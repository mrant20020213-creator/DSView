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

#include "streamrecorder.h"

#include <string.h>
#include <stdio.h>
#include <inttypes.h>
#include <time.h>
#include <zlib.h>
#include <minizip/zip.h>
#include <QDir>
#include <QFileInfo>
#include <QFile>
#include <QDateTime>
#include <libsigrok.h>

#include "dsvdef.h"
#include "log.h"
#include "utility/path.h"

namespace pv {

namespace {
    // Count the bytes written to the file, so the file size is known
    // without waiting for the buffered data to reach the disk.
    struct WriteCounter
    {
        write_file_func  write_func;
        uint64_t        *bytes;
    };

    uLong ZCALLBACK counted_write(voidpf opaque, voidpf stream, const void *buf, uLong size)
    {
        WriteCounter *counter = (WriteCounter*)opaque;
        uLong ret = counter->write_func(NULL, stream, buf, size);
        *counter->bytes += ret;
        return ret;
    }
}

StreamRecorder::StreamRecorder()
{
    _queued_bytes = 0;
    _is_finish = false;
    _recording = false;
    _error = ERR_NONE;
    _error_reported = false;
    _segment_count = 0;
    _total_samples = 0;
    _zip = NULL;
    _carry_len = 0;
    _ch_cursor = 0;
    _fill_bytes = 0;
    _seg_samples = 0;
    _seg_blocks = 0;
    _seg_file_bytes = 0;
    _write_counter = NULL;
}

StreamRecorder::~StreamRecorder()
{
    finish();
    wait();

    if (_write_counter != NULL){
        delete (WriteCounter*)_write_counter;
        _write_counter = NULL;
    }
}

bool StreamRecorder::start(const RecordOptions &opt)
{
    finish();
    wait();

    _opt = opt;
    _error = ERR_NONE;
    _error_reported = false;
    _segment_count = 0;
    _total_samples = 0;
    _queued_bytes = 0;
    _is_finish = false;
    _queue.clear();
    _zip = NULL;
    _carry_len = 0;
    _ch_cursor = 0;
    _fill_bytes = 0;
    _seg_samples = 0;
    _seg_blocks = 0;
    _seg_file_bytes = 0;

    if (_opt.channels.empty() || _opt.dir.isEmpty()){
        set_error(ERR_CREATE_DIR);
        return false;
    }

    QDateTime tm = QDateTime::fromMSecsSinceEpoch(_opt.start_time_ms);
    QString sub_name = "DSView-" + tm.toString("yyyyMMdd-HHmmss");
    QDir root(_opt.dir);
    QString out_dir = root.filePath(sub_name);

    // Avoid to write into a folder that already exists.
    int suffix = 1;
    while (QFileInfo::exists(out_dir)){
        out_dir = root.filePath(sub_name + "-" + QString::number(++suffix));
    }

    if (!root.mkpath(out_dir)){
        dsv_err("StreamRecorder: can't create folder:%s", out_dir.toUtf8().data());
        set_error(ERR_CREATE_DIR);
        return false;
    }

    _out_dir = out_dir;
    _blocks.resize(_opt.channels.size());
    for (auto &b : _blocks){
        b.resize(BlockBytes);
    }

    _recording = true;
    _thread = std::thread(&StreamRecorder::write_proc, this);

    dsv_info("StreamRecorder: start record to:%s", out_dir.toUtf8().data());
    return true;
}

void StreamRecorder::push(const void *data, uint64_t length)
{
    if (!_recording || _error != ERR_NONE || data == NULL || length == 0)
        return;

    {
        std::lock_guard<std::mutex> lock(_mutex);

        if (_is_finish)
            return;

        if (_queued_bytes + length > MaxQueueBytes){
            dsv_err("StreamRecorder: the disk is too slow, %" PRIu64 " bytes are waiting.", _queued_bytes);
            set_error(ERR_QUEUE_OVERFLOW);
            return;
        }

        const uint8_t *p = (const uint8_t*)data;
        _queue.emplace_back(p, p + length);
        _queued_bytes += length;
    }
    _cv.notify_one();
}

void StreamRecorder::finish()
{
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _is_finish = true;
    }
    _cv.notify_one();
}

void StreamRecorder::wait()
{
    if (_thread.joinable())
        _thread.join();
}

bool StreamRecorder::take_error()
{
    if (_error == ERR_NONE)
        return false;

    bool reported = _error_reported.exchange(true);
    return !reported;
}

void StreamRecorder::set_error(int err)
{
    int no_err = ERR_NONE;
    _error.compare_exchange_strong(no_err, err);
}

void StreamRecorder::write_proc()
{
    while (true)
    {
        std::vector<uint8_t> buf;
        {
            std::unique_lock<std::mutex> lock(_mutex);
            _cv.wait(lock, [this]{ return !_queue.empty() || _is_finish; });

            if (_queue.empty())
                break;

            buf = std::move(_queue.front());
            _queue.pop_front();
            _queued_bytes -= buf.size();
        }

        if (_error == ERR_NONE){
            consume(buf.data(), buf.size());
        }
    }

    // Store the samples that not enough to fill a block.
    if (_error == ERR_NONE && _fill_bytes > 0){
        flush_block(_fill_bytes);
    }

    close_segment();

    std::vector<std::vector<uint8_t>>().swap(_blocks);

    // Nothing was stored, remove the empty folder.
    if (_segment_count == 0){
        QDir().rmdir(_out_dir);
    }

    _recording = false;

    dsv_info("StreamRecorder: record end, files:%d, samples:%" PRIu64 ", error:%d",
            (int)_segment_count, (uint64_t)_total_samples, (int)_error);
}

/*
 * The cross data is a stream of 64 bits words, each word have 64 samples
 * of one channel, the channels take turns: ch0, ch1, ... chN, ch0, ...
 * A packet may be ended at any byte.
 */
void StreamRecorder::consume(const uint8_t *data, uint64_t length)
{
    if (_carry_len > 0)
    {
        while (_carry_len < 8 && length > 0){
            _carry[_carry_len++] = *data++;
            length--;
        }

        if (_carry_len < 8)
            return;

        put_word(_carry);
        _carry_len = 0;
    }

    while (length >= 8 && _error == ERR_NONE){
        put_word(data);
        data += 8;
        length -= 8;
    }

    if (_error != ERR_NONE)
        return;

    while (length > 0){
        _carry[_carry_len++] = *data++;
        length--;
    }
}

void StreamRecorder::put_word(const uint8_t *word)
{
    memcpy(_blocks[_ch_cursor].data() + _fill_bytes, word, 8);
    _ch_cursor++;

    if (_ch_cursor == _blocks.size())
    {
        _ch_cursor = 0;
        _fill_bytes += 8;

        if (_fill_bytes == BlockBytes){
            flush_block(BlockBytes);
        }
    }
}

bool StreamRecorder::flush_block(uint64_t bytes)
{
    char name[32];

    _fill_bytes = 0;

    if (_zip == NULL && !open_segment())
        return false;

    uint64_t old_file_bytes = _seg_file_bytes;

    for (size_t i = 0; i < _blocks.size(); i++)
    {
        snprintf(name, sizeof(name), "L-%d/%d", _opt.channels[i].index, _seg_blocks);

        if (!add_entry(name, _blocks[i].data(), bytes)){
            dsv_err("StreamRecorder: write data error, file:%s", _seg_path.toUtf8().data());
            set_error(ERR_WRITE_FILE);
            return false;
        }
    }

    _seg_blocks++;
    _seg_samples += bytes * 8;

    // Switch to a new file if the next block is expected to exceed the limit.
    uint64_t block_file_bytes = _seg_file_bytes - old_file_bytes;

    if (_seg_file_bytes + block_file_bytes > _opt.segment_bytes){
        return close_segment();
    }

    return true;
}

bool StreamRecorder::open_segment()
{
    QString name = QString("seg_%1.dsl").arg((int)_segment_count + 1, 5, 10, QChar('0'));
    _seg_path = QDir(_out_dir).filePath(name);

    auto file_name = path::ConvertPath(_seg_path);

    if (_write_counter == NULL){
        _write_counter = new WriteCounter();
    }

    zlib_filefunc64_def file_func;
    fill_fopen64_filefunc(&file_func);

    WriteCounter *counter = (WriteCounter*)_write_counter;
    counter->write_func = file_func.zwrite_file;
    counter->bytes = &_seg_file_bytes;
    file_func.zwrite_file = counted_write;
    file_func.opaque = counter;

    _seg_file_bytes = 0;
    _zip = zipOpen2_64(file_name.c_str(), 0, NULL, &file_func);

    if (_zip == NULL){
        dsv_err("StreamRecorder: can't create file:%s", _seg_path.toUtf8().data());
        set_error(ERR_CREATE_FILE);
        return false;
    }

    _seg_blocks = 0;
    _seg_samples = 0;
    return true;
}

bool StreamRecorder::close_segment()
{
    if (_zip == NULL)
        return true;

    bool ret = true;

    if (_seg_blocks > 0)
    {
        std::string header = make_header();
        const char *decoders = "[]";

        if (!add_entry("header", header.c_str(), header.size())
            || !add_entry("decoders", decoders, strlen(decoders)))
        {
            ret = false;
        }
    }

    if (zipClose((zipFile)_zip, NULL) != ZIP_OK){
        ret = false;
    }
    _zip = NULL;

    if (_seg_blocks == 0){
        QFile::remove(_seg_path);
    }
    else{
        _segment_count++;
        _total_samples += _seg_samples;
    }

    if (!ret){
        dsv_err("StreamRecorder: close file error:%s", _seg_path.toUtf8().data());
        set_error(ERR_WRITE_FILE);
    }

    _seg_blocks = 0;
    _seg_samples = 0;
    return ret;
}

bool StreamRecorder::add_entry(const char *name, const void *data, uint64_t size)
{
    zip_fileinfo zi;
    memset(&zi, 0, sizeof(zi));

    time_t rawtime;
    time(&rawtime);
    struct tm *ti = localtime(&rawtime);

    if (ti != NULL){
        zi.tmz_date.tm_year = ti->tm_year;
        zi.tmz_date.tm_mon  = ti->tm_mon;
        zi.tmz_date.tm_mday = ti->tm_mday;
        zi.tmz_date.tm_hour = ti->tm_hour;
        zi.tmz_date.tm_min  = ti->tm_min;
        zi.tmz_date.tm_sec  = ti->tm_sec;
    }

    if (zipOpenNewFileInZip((zipFile)_zip, name, &zi, NULL, 0, NULL, 0, NULL,
                Z_DEFLATED, Z_BEST_SPEED) != ZIP_OK)
    {
        return false;
    }

    int ret = zipWriteInFileInZip((zipFile)_zip, data, (unsigned int)size);
    int ret2 = zipCloseFileInZip((zipFile)_zip);

    return ret == ZIP_OK && ret2 == ZIP_OK;
}

std::string StreamRecorder::make_header()
{
    std::string str;
    char meta[300] = {0};

    // The time of the first sample of this file.
    int64_t seg_time = _opt.start_time_ms;
    if (_opt.samplerate > 0){
        seg_time += (int64_t)((double)_total_samples * 1000.0 / (double)_opt.samplerate);
    }

    snprintf(meta, sizeof(meta), "[version]\n"); str += meta;
    snprintf(meta, sizeof(meta), "version = %d\n", HEADER_FORMAT_VERSION); str += meta;
    snprintf(meta, sizeof(meta), "[header]\n"); str += meta;
    snprintf(meta, sizeof(meta), "driver = %s\n", _opt.driver.c_str()); str += meta;
    snprintf(meta, sizeof(meta), "device mode = %d\n", _opt.device_mode); str += meta;
    snprintf(meta, sizeof(meta), "capturefile = data\n"); str += meta;
    snprintf(meta, sizeof(meta), "total samples = %" PRIu64 "\n", _seg_samples); str += meta;
    snprintf(meta, sizeof(meta), "total probes = %d\n", (int)_opt.channels.size()); str += meta;
    snprintf(meta, sizeof(meta), "total blocks = %d\n", _seg_blocks); str += meta;

    char *s = sr_samplerate_string(_opt.samplerate);
    snprintf(meta, sizeof(meta), "samplerate = %s\n", s ? s : "0"); str += meta;
    if (s != NULL){
        g_free(s);
    }

    snprintf(meta, sizeof(meta), "trigger time = %lld\n", (long long)seg_time); str += meta;
    snprintf(meta, sizeof(meta), "trigger pos = 0\n"); str += meta;

    for (auto &ch : _opt.channels){
        snprintf(meta, sizeof(meta), "probe%d = %s\n", ch.index, ch.name.c_str());
        str += meta;
    }

    return str;
}

} // namespace pv
