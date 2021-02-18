/*************************************************************************
 * Copyright (C) [2020] by Cambricon, Inc. All rights reserved
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *************************************************************************/

#include "runner.h"

#include <chrono>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "cxxutil/log.h"

StreamRunner::StreamRunner(const std::string& data_path) : demux_event_handle_(this), data_path_(data_path) {
  parser_.reset(new VideoParser(&demux_event_handle_));
  if (!parser_->Open(data_path.c_str())) {
    THROW_EXCEPTION(edk::Exception::INIT_FAILED, "Open video source failed");
  }

  // set mlu environment
  env_.SetDeviceId(0);
  env_.BindDevice();

  const VideoInfo& info = parser_->GetVideoInfo();
  // create decoder
  edk::EasyDecode::Attr attr;
  attr.frame_geometry.w = info.width;
  attr.frame_geometry.h = info.height;
  attr.codec_type = info.codec_type;
  // attr.interlaced = info.progressive ? false : true;
  attr.pixel_format = edk::PixelFmt::NV21;
  attr.dev_id = 0;
  attr.frame_callback = std::bind(&StreamRunner::ReceiveFrame, this, std::placeholders::_1);
  attr.eos_callback = std::bind(&StreamRunner::ReceiveEos, this);
  attr.silent = false;
  attr.input_buffer_num = 6;
  attr.output_buffer_num = 6;
  decode_ = edk::EasyDecode::New(attr);
}

StreamRunner::~StreamRunner() {
  Stop();
  WaitForRunLoopExit();
}

void StreamRunner::DemuxLoop(const uint32_t repeat_time) {
  bool is_rtsp = parser_->IsRtsp();
  uint32_t loop_time = 0;

  try {
    while (Running()) {
      // frame rate control, 25 frame per second for local video
      int ret = parser_->ParseLoop(is_rtsp ? 0 : 40);
      if (ret == -1) {
        THROW_EXCEPTION(edk::Exception::UNAVAILABLE, "no video source");
      }

      if (ret == 1) {
        // eos
        if (repeat_time > loop_time++) {
          parser_->Close();
          if (!parser_->Open(data_path_.c_str())) {
            THROW_EXCEPTION(edk::Exception::INIT_FAILED, "Open video source failed");
          }
          std::cout << "Loop..." << std::endl;
          continue;
        } else {
          demux_event_handle_.SendEos();
          std::cout << "End Of Stream" << std::endl;
          break;
        }
      }
    }
  } catch (edk::Exception& e) {
    LOGE(SAMPLES) << e.what();
    Stop();
  }
  if (Running()) demux_event_handle_.SendEos();
  parser_->Close();
  std::this_thread::sleep_for(std::chrono::duration<float, std::milli>(1000));

  std::unique_lock<std::mutex> lk(mut_);
  if (frames_.empty()) Stop();
}

bool StreamRunner::RunLoop() {
  in_loop_.store(true);

  try {
    while (running_.load()) {
      // inference
      std::unique_lock<std::mutex> lk(mut_);

      if (!cond_.wait_for(lk, std::chrono::milliseconds(100), [this] { return !frames_.empty(); })) {
        continue;
      }
      edk::CnFrame frame = frames_.front();
      frames_.pop();
      lk.unlock();

      Process(std::move(frame));

      lk.lock();
      if (frames_.size() == 0 && receive_eos_.load()) {
        break;
      }
      lk.unlock();
    }
  } catch (edk::Exception& err) {
    LOGE(SAMPLES) << err.what();
    running_.store(false);
    in_loop_.store(false);
    return false;
  }

  // uninitialize
  running_.store(false);
  in_loop_.store(false);
  return true;
}
