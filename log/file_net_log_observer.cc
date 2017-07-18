// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "net/log/file_net_log_observer.h"

#include <algorithm>
#include <limits>
#include <memory>
#include <queue>
#include <string>
#include <utility>

#include "base/bind.h"
#include "base/files/file_util.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/sequenced_task_runner.h"
#include "base/strings/string_number_conversions.h"
#include "base/synchronization/lock.h"
#include "base/task_scheduler/post_task.h"
#include "base/values.h"
#include "net/log/net_log_capture_mode.h"
#include "net/log/net_log_entry.h"
#include "net/log/net_log_util.h"
#include "net/url_request/url_request_context.h"

// TODO(eroman): Move implementations to match declaration order.

namespace {

// Number of events that can build up in |write_queue_| before a task is posted
// to the file task runner to flush them to disk.
const int kNumWriteQueueEvents = 15;

scoped_refptr<base::SequencedTaskRunner> CreateFileTaskRunner() {
  // The tasks posted to this sequenced task runner do synchronous File I/O for
  // the purposes of writing NetLog files.
  //
  // These intentionally block shutdown to ensure the log file has finished
  // being written.
  return base::CreateSequencedTaskRunnerWithTraits(
      {base::MayBlock(), base::TaskPriority::USER_VISIBLE,
       base::TaskShutdownBehavior::BLOCK_SHUTDOWN});
}

// Opens |path| in write mode. Returns the file handle on success, or nullptr on
// failure.
base::ScopedFILE OpenFileForWrite(const base::FilePath& path) {
  base::ScopedFILE result(base::OpenFile(path, "wb"));
  LOG_IF(ERROR, !result) << "Failed opening: " << path.value();
  return result;
}

// Helper that writes data to a file. The |file| handle may optionally be null,
// in which case nothing will be written. Returns the number of bytes
// successfully written (may be less than input data in case of errors).
size_t WriteToFile(FILE* file,
                   base::StringPiece data1,
                   base::StringPiece data2 = base::StringPiece(),
                   base::StringPiece data3 = base::StringPiece()) {
  size_t bytes_written = 0;

  if (file) {
    // Append each of data1, data2 and data3.
    if (!data1.empty())
      bytes_written += fwrite(data1.data(), 1, data1.size(), file);
    if (!data2.empty())
      bytes_written += fwrite(data2.data(), 1, data2.size(), file);
    if (!data3.empty())
      bytes_written += fwrite(data3.data(), 1, data3.size(), file);
  }

  return bytes_written;
}

// Copies all of the data at |source_path| and appends it to |destination_file|,
// then deletes |source_path|.
void AppendToFileThenDelete(const base::FilePath& source_path,
                            FILE* destination_file,
                            char* read_buffer,
                            size_t read_buffer_size) {
  base::ScopedFILE source_file(base::OpenFile(source_path, "rb"));
  if (!source_file)
    return;

  // Read |source_path|'s contents in chunks of read_buffer_size and append
  // to |destination_file|.
  size_t num_bytes_read;
  while ((num_bytes_read =
              fread(read_buffer, 1, read_buffer_size, source_file.get())) > 0) {
    WriteToFile(destination_file,
                base::StringPiece(read_buffer, num_bytes_read));
  }

  // Now that it has been copied, delete the source file.
  source_file.reset();
  base::DeleteFile(source_path, false);
}

}  // namespace

namespace net {

const size_t FileNetLogObserver::kNoLimit = std::numeric_limits<size_t>::max();

// Used to store events to be written to file.
using EventQueue = std::queue<std::unique_ptr<std::string>>;

// WriteQueue receives events from FileNetLogObserver on the main thread and
// holds them in a queue until they are drained from the queue and written to
// file on the file task runner.
//
// WriteQueue contains the resources shared between the main thread and the
// file task runner. |lock_| must be acquired to read or write to |queue_| and
// |memory_|.
//
// WriteQueue is refcounted and should be destroyed once all events on the
// file task runner have finished executing.
class FileNetLogObserver::WriteQueue
    : public base::RefCountedThreadSafe<WriteQueue> {
 public:
  // |memory_max| indicates the maximum amount of memory that the virtual write
  // queue can use. If |memory_| exceeds |memory_max_|, the |queue_| of events
  // is overwritten.
  explicit WriteQueue(size_t memory_max);

  // Adds |event| to |queue_|. Also manages the size of |memory_|; if it
  // exceeds |memory_max_|, then old events are dropped from |queue_| without
  // being written to file.
  //
  // Returns the number of events in the |queue_|.
  size_t AddEntryToQueue(std::unique_ptr<std::string> event);

  // Swaps |queue_| with |local_queue|. |local_queue| should be empty, so that
  // |queue_| is emptied. Resets |memory_| to 0.
  void SwapQueue(EventQueue* local_queue);

 private:
  friend class base::RefCountedThreadSafe<WriteQueue>;

  ~WriteQueue();

  // Queue of events to be written, shared between main thread and file task
  // runner. Main thread adds events to the queue and the file task runner
  // drains them and writes the events to file.
  //
  // |lock_| must be acquired to read or write to this.
  EventQueue queue_;

  // Tracks how much memory is being used by the virtual write queue.
  // Incremented in AddEntryToQueue() when events are added to the
  // buffer, and decremented when SwapQueue() is called and the file task
  // runner's local queue is swapped with the shared write queue.
  //
  // |lock_| must be acquired to read or write to this.
  size_t memory_;

  // Indicates the maximum amount of memory that the |queue_| is allowed to
  // use.
  const size_t memory_max_;

  // Protects access to |queue_| and |memory_|.
  //
  // A lock is necessary because |queue_| and |memory_| are shared between the
  // file task runner and the main thread. NetLog's lock protects OnAddEntry(),
  // which calls AddEntryToQueue(), but it does not protect access to the
  // observer's member variables. Thus, a race condition exists if a thread is
  // calling OnAddEntry() at the same time that the file task runner is
  // accessing |memory_| and |queue_| to write events to file. The |queue_| and
  // |memory_| counter are necessary to bound the amount of memory that is used
  // for the queue in the event that the file task runner lags significantly
  // behind the main thread in writing events to file.
  base::Lock lock_;

  DISALLOW_COPY_AND_ASSIGN(WriteQueue);
};

// FileWriter is responsible for draining events from a WriteQueue and writing
// them to disk. FileWriter can be constructed on any thread, and
// afterwards is only accessed on the file task runner.
class FileNetLogObserver::FileWriter {
 public:
  // If max_event_file_size == kNoLimit, then no limit is enforced.
  FileWriter(const base::FilePath& log_path,
             size_t max_event_file_size,
             size_t total_num_event_files,
             scoped_refptr<base::SequencedTaskRunner> task_runner);

  ~FileWriter();

  // Writes |constants_value| to disk and opens the events array (closed in
  // Stop()).
  void Initialize(std::unique_ptr<base::Value> constants_value);

  // Closes the events array opened in Initialize() and writes |polled_data| to
  // disk. If |polled_data| cannot be converted to proper JSON, then it
  // is ignored.
  void Stop(std::unique_ptr<base::Value> polled_data);

  // Drains |queue_| from WriteQueue into a local file queue and writes the
  // events in the queue to disk.
  void Flush(scoped_refptr<WriteQueue> write_queue);

  // Deletes all netlog files. It is not valid to call any method of
  // FileNetLogObserver after DeleteAllFiles().
  void DeleteAllFiles();

  void FlushThenStop(scoped_refptr<WriteQueue> write_queue,
                     std::unique_ptr<base::Value> polled_data);

 private:
  // Returns true if there is no file size bound to enforce.
  //
  // When operating in unbounded mode, the implementation is optimized to stream
  // writes to a single file, rather than chunking them across temporary event
  // files.
  bool IsUnbounded() const;
  bool IsBounded() const;

  // Increments |current_event_file_number_|, and updates all state relating to
  // the current event file (open file handle, num bytes written, current file
  // number).
  void IncrementCurrentEventFile();

  // Gets the path to a (temporary) directory where files are written in bounded
  // mode. When logging is stopped these files are stitched together and written
  // to the final log path.
  base::FilePath GetInprogressDirectory() const;

  // Returns the path to the event file having |index|. This looks like
  // "LOGDIR/event_file_<index>.json".
  base::FilePath GetEventFilePath(size_t index) const;

  // Gets the file path where constants are saved at the start of
  // logging. This looks like "LOGDIR/constants.json".
  base::FilePath GetConstantsFilePath() const;

  // Gets the file path where the final data is written at the end of logging.
  // This looks like "LOGDIR/end_netlog.json".
  base::FilePath GetClosingFilePath() const;

  // Returns the corresponding index for |file_number|. File "numbers" are a
  // monotonically increasing identifier that start at 1 (a value of zero means
  // it is uninitialized), whereas the file "index" is a bounded value that
  // wraps and identifies the file path to use.
  //
  // Keeping track of the current number rather than index makes it a bit easier
  // to assemble a file at the end, since it is unambiguous which paths have
  // been used/re-used.
  size_t FileNumberToIndex(size_t file_number) const;

  // Writes |constants_value| to a file.
  static void WriteConstantsToFile(std::unique_ptr<base::Value> constants_value,
                                   FILE* file);

  // Writes |polled_data| to a file.
  static void WritePolledDataToFile(std::unique_ptr<base::Value> polled_data,
                                    FILE* file);

  // If any events were written (wrote_event_bytes_), rewinds |file| by 2 bytes
  // in order to overwrite the trailing ",\n" that was written by the last event
  // line.
  void RewindIfWroteEventBytes(FILE* file) const;

  // Concatenates all the log files to assemble the final
  // |final_log_file_|. This single "stitched" file is what other
  // log ingesting tools expect.
  void StitchFinalLogFile();

  // Creates the .inprogress directory used by bounded mode.
  void CreateInprogressDirectory() const;

  // The path (and associated file handle) where the final netlog is written. In
  // bounded mode this is mostly written to once logging is stopped, whereas in
  // unbounded mode events will be directly written to it.
  const base::FilePath final_log_path_;
  base::ScopedFILE final_log_file_;

  // Holds the file handle for the numbered events file where data is currently
  // being written to. The file path of this file is
  // GetEventFilePath(current_event_file_number_). The
  // file handle may be null if an error previously occurred opening the file,
  // or logging has been stopped.
  base::ScopedFILE current_event_file_;
  size_t current_event_file_size_;

  // Indicates the total number of netlog event files allowed.
  // (The files GetConstantsFilePath() and GetClosingFilePath() do
  // not count against the total.)
  const size_t total_num_event_files_;

  // Counter for the events file currently being written into. See
  // FileNumberToIndex() for an explanation of what "number" vs "index" mean.
  size_t current_event_file_number_;

  // Indicates the maximum size of each individual events file. May be kNoLimit
  // to indicate that it can grow arbitrarily large.
  const size_t max_event_file_size_;

  // Whether any bytes were written for events. This is used to properly format
  // JSON (events list shouldn't end with a comma).
  bool wrote_event_bytes_;

  // Task runner for doing file operations.
  const scoped_refptr<base::SequencedTaskRunner> task_runner_;

  DISALLOW_COPY_AND_ASSIGN(FileWriter);
};

std::unique_ptr<FileNetLogObserver> FileNetLogObserver::CreateBounded(
    const base::FilePath& log_path,
    size_t max_total_size,
    std::unique_ptr<base::Value> constants) {
  // TODO(eroman): Should use something other than 10 for number of files?
  return CreateBoundedInternal(log_path, max_total_size, 10,
                               std::move(constants));
}

std::unique_ptr<FileNetLogObserver> FileNetLogObserver::CreateBoundedInternal(
    const base::FilePath& log_path,
    size_t max_total_size,
    size_t total_num_event_files,
    std::unique_ptr<base::Value> constants) {
  DCHECK_GT(total_num_event_files, 0u);

  scoped_refptr<base::SequencedTaskRunner> file_task_runner =
      CreateFileTaskRunner();

  const size_t max_event_file_size =
      max_total_size == kNoLimit ? kNoLimit
                                 : max_total_size / total_num_event_files;

  // The FileWriter uses a soft limit to write events to file that allows
  // the size of the file to exceed the limit, but the WriteQueue uses a hard
  // limit which the size of |WriteQueue::queue_| cannot exceed. Thus, the
  // FileWriter may write more events to file than can be contained by
  // the WriteQueue if they have the same size limit. The maximum size of the
  // WriteQueue is doubled to allow |WriteQueue::queue_| to hold enough events
  // for the FileWriter to fill all files. As long as all events have
  // sizes <= the size of an individual event file, the discrepancy between the
  // hard limit and the soft limit will not cause an issue.
  // TODO(dconnol): Handle the case when the WriteQueue  still doesn't
  // contain enough events to fill all files, because of very large events
  // relative to file size.
  std::unique_ptr<FileWriter> file_writer(new FileWriter(
      log_path, max_event_file_size, total_num_event_files, file_task_runner));

  scoped_refptr<WriteQueue> write_queue(new WriteQueue(max_total_size * 2));

  return std::unique_ptr<FileNetLogObserver>(
      new FileNetLogObserver(file_task_runner, std::move(file_writer),
                             std::move(write_queue), std::move(constants)));
}

std::unique_ptr<FileNetLogObserver> FileNetLogObserver::CreateUnbounded(
    const base::FilePath& log_path,
    std::unique_ptr<base::Value> constants) {
  return CreateBounded(log_path, kNoLimit, std::move(constants));
}

FileNetLogObserver::~FileNetLogObserver() {
  if (net_log()) {
    // StopObserving was not called.
    net_log()->DeprecatedRemoveObserver(this);
    file_task_runner_->PostTask(
        FROM_HERE, base::Bind(&FileNetLogObserver::FileWriter::DeleteAllFiles,
                              base::Unretained(file_writer_.get())));
  }
  file_task_runner_->DeleteSoon(FROM_HERE, file_writer_.release());
}

void FileNetLogObserver::StartObserving(NetLog* net_log,
                                        NetLogCaptureMode capture_mode) {
  net_log->DeprecatedAddObserver(this, capture_mode);
}

void FileNetLogObserver::StopObserving(std::unique_ptr<base::Value> polled_data,
                                       base::OnceClosure optional_callback) {
  net_log()->DeprecatedRemoveObserver(this);

  base::OnceClosure bound_flush_then_stop =
      base::Bind(&FileNetLogObserver::FileWriter::FlushThenStop,
                 base::Unretained(file_writer_.get()), write_queue_,
                 base::Passed(&polled_data));

  // Note that PostTaskAndReply() requires a non-null closure.
  if (!optional_callback.is_null()) {
    file_task_runner_->PostTaskAndReply(FROM_HERE,
                                        std::move(bound_flush_then_stop),
                                        std::move(optional_callback));
  } else {
    file_task_runner_->PostTask(FROM_HERE, std::move(bound_flush_then_stop));
  }
}

void FileNetLogObserver::OnAddEntry(const NetLogEntry& entry) {
  std::unique_ptr<std::string> json(new std::string);

  // If |entry| cannot be converted to proper JSON, ignore it.
  if (!base::JSONWriter::Write(*entry.ToValue(), json.get()))
    return;

  size_t queue_size = write_queue_->AddEntryToQueue(std::move(json));

  // If events build up in |write_queue_|, trigger the file task runner to drain
  // the queue. Because only 1 item is added to the queue at a time, if
  // queue_size > kNumWriteQueueEvents a task has already been posted, or will
  // be posted.
  if (queue_size == kNumWriteQueueEvents) {
    file_task_runner_->PostTask(
        FROM_HERE,
        base::Bind(&FileNetLogObserver::FileWriter::Flush,
                   base::Unretained(file_writer_.get()), write_queue_));
  }
}

std::unique_ptr<FileNetLogObserver> FileNetLogObserver::CreateBoundedForTests(
    const base::FilePath& log_path,
    size_t max_total_size,
    size_t total_num_event_files,
    std::unique_ptr<base::Value> constants) {
  return CreateBoundedInternal(log_path, max_total_size, total_num_event_files,
                               std::move(constants));
}

FileNetLogObserver::FileNetLogObserver(
    scoped_refptr<base::SequencedTaskRunner> file_task_runner,
    std::unique_ptr<FileWriter> file_writer,
    scoped_refptr<WriteQueue> write_queue,
    std::unique_ptr<base::Value> constants)
    : file_task_runner_(std::move(file_task_runner)),
      write_queue_(std::move(write_queue)),
      file_writer_(std::move(file_writer)) {
  if (!constants)
    constants = GetNetConstants();
  file_task_runner_->PostTask(
      FROM_HERE, base::Bind(&FileNetLogObserver::FileWriter::Initialize,
                            base::Unretained(file_writer_.get()),
                            base::Passed(&constants)));
}

FileNetLogObserver::WriteQueue::WriteQueue(size_t memory_max)
    : memory_(0), memory_max_(memory_max) {}

size_t FileNetLogObserver::WriteQueue::AddEntryToQueue(
    std::unique_ptr<std::string> event) {
  base::AutoLock lock(lock_);

  memory_ += event->size();
  queue_.push(std::move(event));

  while (memory_ > memory_max_ && !queue_.empty()) {
    // Delete oldest events in the queue.
    DCHECK(queue_.front());
    memory_ -= queue_.front()->size();
    queue_.pop();
  }

  return queue_.size();
}

void FileNetLogObserver::WriteQueue::SwapQueue(EventQueue* local_queue) {
  DCHECK(local_queue->empty());
  base::AutoLock lock(lock_);
  queue_.swap(*local_queue);
  memory_ = 0;
}

FileNetLogObserver::WriteQueue::~WriteQueue() {}

void FileNetLogObserver::FileWriter::FlushThenStop(
    scoped_refptr<FileNetLogObserver::WriteQueue> write_queue,
    std::unique_ptr<base::Value> polled_data) {
  Flush(write_queue);
  Stop(std::move(polled_data));
}

FileNetLogObserver::FileWriter::FileWriter(
    const base::FilePath& log_path,
    size_t max_event_file_size,
    size_t total_num_event_files,
    scoped_refptr<base::SequencedTaskRunner> task_runner)
    : final_log_path_(log_path),
      total_num_event_files_(total_num_event_files),
      current_event_file_number_(0),
      max_event_file_size_(max_event_file_size),
      wrote_event_bytes_(false),
      task_runner_(std::move(task_runner)) {}

FileNetLogObserver::FileWriter::~FileWriter() {}

void FileNetLogObserver::FileWriter::Initialize(
    std::unique_ptr<base::Value> constants_value) {
  DCHECK(task_runner_->RunsTasksInCurrentSequence());

  // Open the final log file, and keep it open for the duration of logging (even
  // in bounded mode).
  final_log_file_ = OpenFileForWrite(final_log_path_);

  if (IsBounded()) {
    CreateInprogressDirectory();
    base::ScopedFILE constants_file = OpenFileForWrite(GetConstantsFilePath());
    WriteConstantsToFile(std::move(constants_value), constants_file.get());
  } else {
    WriteConstantsToFile(std::move(constants_value), final_log_file_.get());
  }
}

void FileNetLogObserver::FileWriter::WriteConstantsToFile(
    std::unique_ptr<base::Value> constants_value,
    FILE* file) {
  // Print constants to file and open events array.
  std::string json;

  // It should always be possible to convert constants to JSON.
  if (!base::JSONWriter::Write(*constants_value, &json))
    DCHECK(false);
  WriteToFile(file, "{\"constants\":", json, ",\n\"events\": [\n");
}

void FileNetLogObserver::FileWriter::CreateInprogressDirectory() const {
  DCHECK(IsBounded());

  // base::CreateDirectory() creates missing parent directories. Since the
  // target directory is a sibling to |final_log_path_|, if that file couldn't
  // be opened don't attempt to create the directory either.
  if (!final_log_file_)
    return;

  if (!base::CreateDirectory(GetInprogressDirectory())) {
    LOG(WARNING) << "Failed creating directory: "
                 << GetInprogressDirectory().value();
    return;
  }

  // Since |final_log_file_| will not be written to until the very end, leave
  // some data in it explaining that the real data is currently in the
  // .inprogress directory. This ordinarily won't be visible (overwritten when
  // stopping) however if logging does not end gracefully the comments are
  // useful for recovery.
  //
  // TODO(eroman): Give a better description, including instructions on how
  // to stitch the files manually if logging did not end gracefully.
  WriteToFile(final_log_file_.get(),
              "Log data is being written to the XXX.inprogress directory");
  fflush(final_log_file_.get());
}

void FileNetLogObserver::FileWriter::Stop(
    std::unique_ptr<base::Value> polled_data) {
  DCHECK(task_runner_->RunsTasksInCurrentSequence());

  // Write out the polled data.
  if (IsBounded()) {
    base::ScopedFILE closing_file = OpenFileForWrite(GetClosingFilePath());
    WritePolledDataToFile(std::move(polled_data), closing_file.get());
  } else {
    RewindIfWroteEventBytes(final_log_file_.get());
    WritePolledDataToFile(std::move(polled_data), final_log_file_.get());
  }

  // If operating in bounded mode, the events were written to separate files
  // within GetInprogressDirectory(). Assemble them into the final destination
  // file.
  if (IsBounded())
    StitchFinalLogFile();

  // Ensure the final log file has been flushed.
  final_log_file_.reset();
}

void FileNetLogObserver::FileWriter::WritePolledDataToFile(
    std::unique_ptr<base::Value> polled_data,
    FILE* file) {
  // Close the events array.
  WriteToFile(file, "]");

  // Write the polled data (if any).
  if (polled_data) {
    std::string polled_data_json;
    base::JSONWriter::Write(*polled_data, &polled_data_json);
    if (!polled_data_json.empty())
      WriteToFile(file, ",\n\"polledData\": ", polled_data_json, "\n");
  }

  // Close the log.
  WriteToFile(file, "}\n");
}

bool FileNetLogObserver::FileWriter::IsUnbounded() const {
  return max_event_file_size_ == kNoLimit;
}

bool FileNetLogObserver::FileWriter::IsBounded() const {
  return !IsUnbounded();
}

void FileNetLogObserver::FileWriter::IncrementCurrentEventFile() {
  DCHECK(task_runner_->RunsTasksInCurrentSequence());
  DCHECK(IsBounded());

  current_event_file_number_++;
  current_event_file_ = OpenFileForWrite(
      GetEventFilePath(FileNumberToIndex(current_event_file_number_)));
  current_event_file_size_ = 0;
}

base::FilePath FileNetLogObserver::FileWriter::GetInprogressDirectory() const {
  return final_log_path_.AddExtension(FILE_PATH_LITERAL(".inprogress"));
}

base::FilePath FileNetLogObserver::FileWriter::GetEventFilePath(
    size_t index) const {
  DCHECK_LT(index, total_num_event_files_);
  DCHECK(IsBounded());
  return GetInprogressDirectory().AppendASCII(
      "event_file_" + base::SizeTToString(index) + ".json");
}

base::FilePath FileNetLogObserver::FileWriter::GetConstantsFilePath() const {
  return GetInprogressDirectory().AppendASCII("constants.json");
}

base::FilePath FileNetLogObserver::FileWriter::GetClosingFilePath() const {
  return GetInprogressDirectory().AppendASCII("end_netlog.json");
}

void FileNetLogObserver::FileWriter::Flush(
    scoped_refptr<FileNetLogObserver::WriteQueue> write_queue) {
  DCHECK(task_runner_->RunsTasksInCurrentSequence());

  EventQueue local_file_queue;
  write_queue->SwapQueue(&local_file_queue);

  while (!local_file_queue.empty()) {
    FILE* output_file;

    // If in bounded mode, output events to the current event file. Otherwise
    // output events to the final log path.
    if (IsBounded()) {
      if (current_event_file_number_ == 0 ||
          current_event_file_size_ >= max_event_file_size_) {
        IncrementCurrentEventFile();
      }
      output_file = current_event_file_.get();
    } else {
      output_file = final_log_file_.get();
    }

    size_t bytes_written =
        WriteToFile(output_file, *local_file_queue.front(), ",\n");

    wrote_event_bytes_ |= bytes_written > 0;

    // Keep track of the filesize for current event file when in bounded mode.
    if (IsBounded())
      current_event_file_size_ += bytes_written;

    local_file_queue.pop();
  }
}

void FileNetLogObserver::FileWriter::DeleteAllFiles() {
  DCHECK(task_runner_->RunsTasksInCurrentSequence());

  final_log_file_.reset();

  if (IsBounded()) {
    current_event_file_.reset();
    base::DeleteFile(GetInprogressDirectory(), true);
  }

  base::DeleteFile(final_log_path_, false);
}

size_t FileNetLogObserver::FileWriter::FileNumberToIndex(
    size_t file_number) const {
  DCHECK_GT(file_number, 0u);
  // Note that "file numbers" start at 1 not 0.
  return (file_number - 1) % total_num_event_files_;
}

void FileNetLogObserver::FileWriter::RewindIfWroteEventBytes(FILE* file) const {
  if (file && wrote_event_bytes_) {
    // To be valid JSON the events array should not end with a comma. If events
    // were written though, they will have been terminated with "\n," so strip
    // it before closing the events array.
    fseek(file, -2, SEEK_END);
  }
}

void FileNetLogObserver::FileWriter::StitchFinalLogFile() {
  // Make sure all the events files are flushed (as will read them next).
  current_event_file_.reset();

  // Allocate a 64K buffer used for reading the files. At most kReadBufferSize
  // bytes will be in memory at a time.
  const size_t kReadBufferSize = 1 << 16;  // 64KiB
  std::unique_ptr<char[]> read_buffer(new char[kReadBufferSize]);

  // Re-open the final log file in order to truncate it.
  final_log_file_ = OpenFileForWrite(final_log_path_);

  // Append the constants file.
  AppendToFileThenDelete(GetConstantsFilePath(), final_log_file_.get(),
                         read_buffer.get(), kReadBufferSize);

  // Iterate over the events files, from oldest to most recent, and append them
  // to the final destination. Note that "file numbers" start at 1 not 0.
  size_t end_filenumber = current_event_file_number_ + 1;
  size_t begin_filenumber = current_event_file_number_ <= total_num_event_files_
                                ? 1
                                : end_filenumber - total_num_event_files_;
  for (size_t filenumber = begin_filenumber; filenumber < end_filenumber;
       ++filenumber) {
    AppendToFileThenDelete(GetEventFilePath(FileNumberToIndex(filenumber)),
                           final_log_file_.get(), read_buffer.get(),
                           kReadBufferSize);
  }

  // Account for the final event line ending in a ",\n". Strip it to form valid
  // JSON.
  RewindIfWroteEventBytes(final_log_file_.get());

  // Append the polled data.
  AppendToFileThenDelete(GetClosingFilePath(), final_log_file_.get(),
                         read_buffer.get(), kReadBufferSize);

  // Delete the inprogress directory (and anything that may still be left inside
  // it).
  base::DeleteFile(GetInprogressDirectory(), true);
}

}  // namespace net
