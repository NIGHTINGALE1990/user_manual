/****************************************************************************
 *
 *   Copyright (c) 2017-2019 PX4 Development Team. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name PX4 nor the names of its contributors may be
 *    used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/

#include "NRA24.hpp"
#include <systemlib/mavlink_log.h>
extern orb_advert_t mavlink_log_pub;

NRA24::NRA24(const char *port, uint8_t rotation) :
	ScheduledWorkItem(MODULE_NAME, px4::wq_configurations::hp_default),
	_px4_rangefinder(0 /* TODO: device id */, ORB_PRIO_DEFAULT, rotation)
{
	// store port name
	strncpy(_port, port, sizeof(_port) - 1);

	// enforce null termination
	_port[sizeof(_port) - 1] = '\0';
}

NRA24::~NRA24()
{
	// make sure we are truly inactive
	stop();

	perf_free(_sample_perf);
	perf_free(_comms_errors);
}

int
NRA24::init()
{
	int32_t hw_model = 1; // only one model so far...

	switch (hw_model) {
	case 1: // NRA24 (12m, 100 Hz)
		_px4_rangefinder.set_min_distance(NRA24_MIN_DISTANCE);
		_px4_rangefinder.set_max_distance(NRA24_MAX_DISTANCE);
		_px4_rangefinder.set_fov(math::radians(18.0f));
		distance_track = 0.0f;

		break;

	default:
		PX4_ERR("invalid HW model %d.", hw_model);
		return -1;
	}

	// status
	int ret = 0;

	do { // create a scope to handle exit conditions using break

		// open fd
		_fd = ::open(_port, O_RDWR | O_NOCTTY);

		if (_fd < 0) {
			PX4_ERR("Error opening fd");
			return -1;
		}

		// baudrate 115200, 8 bits, no parity, 1 stop bit
		unsigned speed = B115200;
		termios uart_config{};
		int termios_state{};

		tcgetattr(_fd, &uart_config);

		// clear ONLCR flag (which appends a CR for every LF)
		uart_config.c_oflag &= ~ONLCR;

		// set baud rate
		if ((termios_state = cfsetispeed(&uart_config, speed)) < 0) {
			PX4_ERR("CFG: %d ISPD", termios_state);
			ret = -1;
			break;
		}

		if ((termios_state = cfsetospeed(&uart_config, speed)) < 0) {
			PX4_ERR("CFG: %d OSPD\n", termios_state);
			ret = -1;
			break;
		}

		if ((termios_state = tcsetattr(_fd, TCSANOW, &uart_config)) < 0) {
			PX4_ERR("baud %d ATTR", termios_state);
			ret = -1;
			break;
		}

		uart_config.c_cflag |= (CLOCAL | CREAD);	// ignore modem controls
		uart_config.c_cflag &= ~CSIZE;
		uart_config.c_cflag |= CS8;			// 8-bit characters
		uart_config.c_cflag &= ~PARENB;			// no parity bit
		uart_config.c_cflag &= ~CSTOPB;			// only need 1 stop bit
		uart_config.c_cflag &= ~CRTSCTS;		// no hardware flowcontrol

		// setup for non-canonical mode
		uart_config.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON);
		uart_config.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
		uart_config.c_oflag &= ~OPOST;

		// fetch bytes as they become available
		uart_config.c_cc[VMIN] = 1;
		uart_config.c_cc[VTIME] = 1;

		if (_fd < 0) {
			PX4_ERR("FAIL: laser fd");
			ret = -1;
			break;
		}
	} while (0);

	// close the fd
	::close(_fd);
	_fd = -1;

	if (ret == PX4_OK) {
		start();
	}

	return ret;
}

int
NRA24::collect()
{
	perf_begin(_sample_perf);

	// clear buffer if last read was too long ago
	int64_t read_elapsed = hrt_elapsed_time(&_last_read);

	// the buffer for read chars is buflen minus null termination
	char readbuf[sizeof(_linebuf)] {};
	unsigned readlen = sizeof(readbuf) - 1;
	int ret = 0;
	float distance_m = -1.0f;
	int roll_count = -1;

	// Check the number of bytes available in the buffer
	int bytes_available = 0;
	::ioctl(_fd, FIONREAD, (unsigned long)&bytes_available);

	if (!bytes_available) {
		perf_end(_sample_perf);
		return -EAGAIN;
	}

	// parse entire buffer
	const hrt_abstime timestamp_sample = hrt_absolute_time();

	do {
		// read from the sensor (uart buffer)
		ret = ::read(_fd, &readbuf[0], readlen);
		if (ret < 0) {
			PX4_ERR("read err: %d", ret);
			perf_count(_comms_errors);
			perf_end(_sample_perf);

			// only throw an error if we time out
			if (read_elapsed > (NRA24_MEASURE_INTERVAL * 3)) {
				/* flush anything in RX buffer */
				tcflush(_fd, TCIFLUSH);
				PX4_INFO("flush anything in RX buffer");

				return ret;

			} else {
				return -EAGAIN;
			}
		}

		_last_read = hrt_absolute_time();

		// parse buffer
		for (int i = 0; i < ret; i++) {
			nra24_parse(readbuf[i], _linebuf, &_linebuf_index, &_parse_state, &distance_m, &roll_count);
		}

		// mavlink_log_info(&mavlink_log_pub, "Nra24 roll_count------- %d\r\n", (int)roll_count);

		// bytes left to parse
		bytes_available -= ret;

	} while (bytes_available > 0);

	// no valid measurement after parsing buffer
	if (distance_m < 0.0f) {
		perf_end(_sample_perf);
		return -EAGAIN;
	}

	float _distance_temp;
	_distance_temp = distance_track + 0.65f * (distance_m - distance_track);
	distance_track = _distance_temp;

	// mavlink_log_info(&mavlink_log_pub, "Nra24 current distance------- %.2f m\r\n", (double)distance_track);

	// publish most recent valid measurement from buffer
	_px4_rangefinder.update(timestamp_sample, distance_track);

	perf_end(_sample_perf);

	return PX4_OK;
}

void
NRA24::start()
{
	// schedule a cycle to start things
	ScheduleOnInterval(100_us);
}

void
NRA24::stop()
{
	ScheduleClear();
}

void
NRA24::Run()
{
	// fds initialized?
	if (_fd < 0) {
		// open fd
		_fd = ::open(_port, O_RDWR | O_NOCTTY);
	}

	// perform collection
	if (collect() == -EAGAIN) {
		// reschedule to grab the missing bits, time to transmit 9 bytes @ 115200 bps
		ScheduleClear();
		ScheduleOnInterval(100_us, 87 * 9);
		return;
	}
}

void
NRA24::print_info()
{
	printf("Using port '%s'\n", _port);
	perf_print_counter(_sample_perf);
	perf_print_counter(_comms_errors);

	_px4_rangefinder.print_status();
}
