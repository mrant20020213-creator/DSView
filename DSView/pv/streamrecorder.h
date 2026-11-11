/*
 * This file is part of the DSView project.
 * DSView is based on PulseView.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
 */

#ifndef DSVIEW_PV_STREAMRECORDER_H
#define DSVIEW_PV_STREAMRECORDER_H

#include <stdint.h>
#include <string>
#include <vector>
#include <deque>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <QString>

namespace pv {

struct RecordChannel
{
    int         index;
    std::string name;
};

struct RecordOptions
{
    QString     dir;            // The folder selected by the user.
    uint64_t    segment_bytes;  // Switch to a new file when the current one reaches this size.
    std::string driver;
    int         device_mode;
    uint64_t    samplerate;
    int64_t     start_time_ms;  // Milliseconds since epoch of the first sample.
    std::vector<RecordChannel> channels; // The enabled logic channels, in data order.
};

/*
 * Writes the logic data stream to disk while capturing.
 * The data is stored as a sequence of .dsl files, each of them can be
 * opened by DSView. All disk work is done on a private thread, the
 * device thread only copies the packet into a queue.
 */
class StreamRecorder
{
public:
    enum RECORD_ERROR
    {
        ERR_NONE = 0,
        ERR_CREATE_DIR = 1,
        ERR_CREATE_FILE = 2,
        ERR_WRITE_FILE = 3,
        ERR_QUEUE_OVERFLOW = 4,
    };

    // The max bytes waiting to be written, the record fails when exceeded.
    static const uint64_t MaxQueueBytes = 512ULL * 1024 * 1024;
    // The bytes of one data block of a channel, the same as LogicSnapshot::LeafBlockSamples / 8.
    static const uint64_t BlockBytes = 1ULL << 21;

    StreamRecorder();
    ~StreamRecorder();

    // Create the output folder and start the writer thread.
    bool start(const RecordOptions &opt);

    // Append the cross format logic data, called from the device thread.
    void push(const void *data, uint64_t length);

    // No more data, the writer thread flushes the queue and closes the file. Non-blocking.
    void finish();

    // Wait for the writer thread to exit.
    void wait();

    inline bool is_recording(){
        return _recording;
    }

    inline int error(){
        return _error;
    }

    // Return true only once after an error occurred.
    bool take_error();

    inline QString output_dir(){
        return _out_dir;
    }

    inline int segment_count(){
        return _segment_count;
    }

    inline uint64_t total_samples(){
        return _total_samples;
    }

private:
    void write_proc();
    void consume(const uint8_t *data, uint64_t length);
    void put_word(const uint8_t *word);
    bool flush_block(uint64_t bytes);
    bool open_segment();
    bool close_segment();
    bool add_entry(const char *name, const void *data, uint64_t size);
    std::string make_header();
    void set_error(int err);

private:
    RecordOptions   _opt;
    QString         _out_dir;
    QString         _seg_path;

    std::thread     _thread;
    std::mutex      _mutex;
    std::condition_variable _cv;
    std::deque<std::vector<uint8_t>> _queue;
    uint64_t        _queued_bytes;
    bool            _is_finish;

    std::atomic<bool>   _recording;
    std::atomic<int>    _error;
    std::atomic<bool>   _error_reported;
    std::atomic<int>    _segment_count;
    std::atomic<uint64_t> _total_samples;

    // Used by the writer thread only.
    void            *_zip;
    std::vector<std::vector<uint8_t>> _blocks;
    uint8_t         _carry[8];
    int             _carry_len;
    size_t          _ch_cursor;
    uint64_t        _fill_bytes;
    uint64_t        _seg_samples;
    int             _seg_blocks;
    uint64_t        _seg_file_bytes;
    void            *_write_counter;
};

} // namespace pv

#endif
