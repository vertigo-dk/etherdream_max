/*Ændret linje 720 den låser tråd automatisk*/

/* Ether Dream interface library
*
* Copyright 2011-2012 Jacob Potter
*
* This program is free software: you can redistribute it and/or modify
* it under the terms of either the GNU General Public License version 2
* or 3, or the GNU Lesser General Public License version 3, as published
* by the Free Software Foundation, at your option.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/
#include "netherdream.h"

static FILE* trace_fp = NULL;
static std::mutex dac_list_lock;
static struct netherdream* dac_list = NULL;
static std::chrono::high_resolution_clock::time_point startTime = std::chrono::high_resolution_clock::now();
static std::thread watcherThread;

/* microseconds()
*
* Return the number of microseconds since library initialization.
*/
static long long microseconds(void) {
	const auto upTime = std::chrono::high_resolution_clock::now() - startTime;
	return std::chrono::duration_cast<std::chrono::microseconds>(upTime).count();
}

/* microsleep(us)
*
* Like usleep().
*/
static void microsleep(long long us) {
	std::this_thread::sleep_for(std::chrono::microseconds(us));
}

/* trace(d, fmt, ...)
*
* Utility function for logging.
*/
static void trace(struct netherdream* d, const char* fmt, ...) {
	if (!trace_fp)
		return;

	char buf[120];

	long long v = microseconds();
	int len;

	if (d)
		len = snprintf(buf, sizeof buf, "[%d.%06d] %06lx ",
			(int)(v / 1000000), (int)(v % 1000000), d->dac_id);
	else
		len = snprintf(buf, sizeof buf, "[%d.%06d]        ",
			(int)(v / 1000000), (int)(v % 1000000));

	va_list args;
	va_start(args, fmt);
	vsnprintf(buf + len, sizeof buf - len, fmt, args);
	va_end(args);

	fputs(buf, trace_fp);
}

/* log_socket_error(d, call)
*
* Log an error in a socket call.
*/
static void log_socket_error(struct netherdream* d, const char* call) {
	trace(d, "!! socket error in %s: %d: %s\n",
		call, errno, strerror(errno));
}

/* wait_for_fd_activity(d, usec, writable)
*
* Wait for activity (if writable is 0, then readable or error; if writable
* is 1, then writable or error) on d's socket. Time out after usec. Returns
* 1 if activity happened, 0 on timeout, -1 on error (will also log error).
*/
static int wait_for_fd_activity(struct netherdream* d, int usec, int writable) {
	fd_set set;
	FD_ZERO(&set);
	FD_SET(d->conn.dc_sock, &set);
	struct timeval t;
	t.tv_sec = usec / 1000000;
	t.tv_usec = usec % 1000000;
	int res = select(d->conn.dc_sock + 1, (writable ? NULL : &set),
		(writable ? &set : NULL), &set, &t);
	if (res < 0)
		log_socket_error(d, "select");

	return res;
}

/* read_bytes(d, buf, len)
*
* Read exactly len bytes from d's connection socket into buf. Returns 0 on
* success, -1 on error (will also log error).
*/
static int read_bytes(struct netherdream* d, char* buf, int len) {
	while (d->conn.dc_read_buf_size < len) {
		int res = wait_for_fd_activity(d, DEFAULT_TIMEOUT, 0);
		if (res < 0)
			return res;
		if (res == 0) {
			trace(d, "!! Read from DAC timed out.\n");
			return -1;
		}

		res = recv(d->conn.dc_sock,
			d->conn.dc_read_buf + d->conn.dc_read_buf_size,
			len - d->conn.dc_read_buf_size, 0);

		if (res <= 0) {
			log_socket_error(d, "recv");
			return -1;
		}

		d->conn.dc_read_buf_size += res;
	}

	memcpy(buf, d->conn.dc_read_buf, len);
	if (d->conn.dc_read_buf_size > len) {
		printf("moving %d up by %d\n", d->conn.dc_read_buf_size, len);
		memmove(d->conn.dc_read_buf, d->conn.dc_read_buf + len,
			d->conn.dc_read_buf_size - len);
	}
	d->conn.dc_read_buf_size -= len;

	return 0;
}

/* send_all(d, data, len)
*
* Send all of data to d's socket. Returns 0 on success, -1 on error or if the
* send times out (will also log error).
*/
static int send_all(struct netherdream* d, const char* data, int len) {
	do {
		int res = wait_for_fd_activity(d, 100000, 1);
		if (res < 0)
			return -1;
		if (res == 0) {
			trace(d, "write timed out\n");
		}

		res = send(d->conn.dc_sock, data, len, 0);

		if (res < 0) {
			log_socket_error(d, "send");
			return -1;
		}

		len -= res;
		data += res;
	} while (len);

	return 0;
}

/* read_resp(d)
*
* Read a response from the DAC into d's conn.resp buffer. Returns 0 on
* success, -1 on error (in which case the error will have been logged).
*/
static int read_resp(struct netherdream* d) {
	int res = read_bytes(d, (char*)&d->conn.resp, sizeof(d->conn.resp));
	if (res < 0)
		return res;

	d->conn.dc_last_ack_time = microseconds();
	return 0;
}

/* dump_resp(d)
*
* Dump the last response received from d.
*/
static void dump_resp(struct netherdream* d) {
	struct etherdream_conn* conn = &d->conn;
	struct dac_status* st = &conn->resp.dac_status;
	trace(d, "-- Protocol %d / LE %d / playback %d / source %d\n",
		0 /* st->protocol */, st->light_engine_state,
		st->playback_state, st->source);
	trace(d, "-- Flags: LE %x, playback %x, source %x\n",
		st->light_engine_flags, st->playback_flags,
		st->source_flags);
	trace(d, "-- Buffer: %d points, %d pps, %d total played\n",
		st->buffer_fullness, st->point_rate, st->point_count);
}

/* dac_connect(d, host, port)
*
* Initialize a dac's connection struct and open up a socket. On success,
* return 0; otherwise, return -1.
*/
static int dac_connect(struct netherdream* d) {
	struct etherdream_conn* conn = &d->conn;
	memset(conn, 0, sizeof * conn);

	// Open socket
	conn->dc_sock = socket(AF_INET, SOCK_STREAM, 0);

	if (conn->dc_sock < 0) {
		log_socket_error(d, "socket");
		return -1;
	}

	u_long iMode = 1;
#ifdef _MSC_VER
	ioctlsocket(conn->dc_sock, FIONBIO, &iMode);
#else
	ioctl(conn->dc_sock, FIONBIO, &iMode);
#endif

	sockaddr_in addr;
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = d->addr.s_addr;
	addr.sin_port = htons(7765);

	connect(conn->dc_sock, (struct sockaddr*)&addr, (int)sizeof addr);

#ifdef _MSC_VER
	// Because the socket is nonblocking, this will always error...
	if (WSAGetLastError() != WSAEWOULDBLOCK) {
#else
	if (errno != EINPROGRESS) {
#endif
		log_socket_error(d, "connect");
		goto bail;
	}



	// Wait for connection to go through
	{
		int res = wait_for_fd_activity(d, DEFAULT_TIMEOUT, 1);
		if (res < 0)
			goto bail;
		if (res == 0) {
			trace(d, "Connection to %s timed out.\n", inet_ntoa(d->addr));
			goto bail;
		}
	}

	// See if we have *actually* connected
	{
		int error;
#ifdef _MSC_VER
		int len = sizeof error;
#else 
		unsigned int len = sizeof error;
#endif
		if (getsockopt(conn->dc_sock, SOL_SOCKET, SO_ERROR, (char*)&error,
			&len) < 0) {
			log_socket_error(d, "getsockopt");
			goto bail;
		}

		if (error) {
			errno = error;
			log_socket_error(d, "connect");
			goto bail;
		}
	}

	{
		int ndelay = 1;
		if (setsockopt(conn->dc_sock, IPPROTO_TCP, TCP_NODELAY,
			(char*)&ndelay, sizeof(ndelay)) < 0) {
			log_socket_error(d, "setsockopt TCP_NODELAY");
			goto bail;
		}
	}

	// After we connect, the DAC will send an initial status response
	if (read_resp(d) < 0)
		goto bail;

	{
		char c = 'p';
		send_all(d, &c, 1);
	}

	if (read_resp(d) < 0)
		goto bail;
	dump_resp(d);

	if (d->sw_revision >= 2) {
		char c = 'v';
		if (send_all(d, &c, 1) < 0)
			goto bail;
		int res = read_bytes(d, d->version, sizeof(d->version));
		if (res < 0)
			return res;
	}
	else {
		strcpy(d->version, "[old]");
	}

	trace(d, "DAC version %.*s\n", sizeof(d->version), d->version);
	return 0;

bail:
#ifdef _MSC_VER
	closesocket(d->conn.dc_sock);
#else
	close(d->conn.dc_sock);
#endif
	return -1;
	}

/* check_data_response(d)
*
* Handle a response from d: update our record of the number of sent-but-not-
* ACKed points, and error if the response was unexpected.
*/
static int check_data_response(struct netherdream* d) {
	struct etherdream_conn* conn = &d->conn;
	if (conn->resp.dac_status.playback_state == 0)
		conn->dc_begin_sent = 0;

	if (conn->resp.command == 'd') {
		if (conn->ackbuf_prod == conn->ackbuf_cons) {
			trace(d, "!! protocol error: unexpected data ack\n");
			return -1;
		}
		conn->unacked_points -= conn->ackbuf[conn->ackbuf_cons];
		conn->ackbuf_cons = (conn->ackbuf_cons + 1) % MAX_LATE_ACKS;
	}
	else {
		conn->pending_meta_acks--;
	}

	if (conn->resp.response != 'a' && conn->resp.response != 'I') {
		trace(d, "!! protocol error: ACK for '%c' got '%c' (%d)\n",
			conn->resp.command,
			conn->resp.response, conn->resp.response);
		return -1;
	}

	return 0;
}

/* dac_get_acks(d, wait)
*
* Read any ACKs we are owed, waiting up to 'wait' microseconds.
*/
static int dac_get_acks(struct netherdream* d, int wait) {
	while (d->conn.pending_meta_acks
		|| (d->conn.ackbuf_prod != d->conn.ackbuf_cons)) {
		int res = wait_for_fd_activity(d, wait, 0);
		if (res <= 0)
			return res;
		if ((res = read_resp(d)) < 0)
			return res;
		if ((res = check_data_response(d)) < 0)
			return res;
	}
	return 0;
}

/* dac_send_data(d, data, npoints, rate)
*
* Send points to the DAC, including prepare or begin commands and changing
* the point rate as necessary.
*/
static int dac_send_data(struct netherdream* d, struct dac_point* data,
	int npoints, int rate) {
	int res;
	const struct dac_status* st = &d->conn.resp.dac_status;

	if (st->playback_state == 0) {
		trace(d, "L: Sending prepare command...\n");
		char c = 'p';
		if ((res = send_all(d, &c, sizeof c)) < 0)
			return res;

		d->conn.pending_meta_acks++;

		/* Block here until all ACKs received... XXX timeout */
		while (d->conn.pending_meta_acks)
			dac_get_acks(d, 1500);

		trace(d, "L: prepare ACKed\n");
	}

	if (st->buffer_fullness > 1600 && st->playback_state == 1 \
		&& !d->conn.dc_begin_sent) {
		trace(d, "L: Sending begin command...\n");

		struct begin_command b = {
			'b',
			0,
			(uint32_t)rate
		};
		if ((res = send_all(d, (const char*)&b, sizeof b)) < 0)
			return res;

		d->conn.dc_begin_sent = 1;
		d->conn.pending_meta_acks++;
	}

	if ((res = dac_get_acks(d, 0)) < 0)
		return res;

	if (npoints <= 0)
		return 0;

	d->conn.dc_local_buffer.queue.command = 'q';
	d->conn.dc_local_buffer.queue.point_rate = rate;

	d->conn.dc_local_buffer.header.command = 'd';
	d->conn.dc_local_buffer.header.npoints = npoints;

	memcpy(&d->conn.dc_local_buffer.data[0], data,
		npoints * sizeof(struct dac_point));

	d->conn.dc_local_buffer.data[0].control |= DAC_CTRL_RATE_CHANGE;

	/* Write the data */
	if ((res = send_all(d, (const char*)&d->conn.dc_local_buffer,
		8 + npoints * sizeof(struct dac_point))) < 0)
		return res;

	/* Expect two ACKs */
	d->conn.pending_meta_acks++;
	d->conn.ackbuf[d->conn.ackbuf_prod] = npoints;
	d->conn.ackbuf_prod = (d->conn.ackbuf_prod + 1) % MAX_LATE_ACKS;
	d->conn.unacked_points += npoints;

	return 0;
}

#define SHOULD_TRACE() (expected_fullness < DEBUG_THRESHOLD_POINTS \
           || d->conn.resp.dac_status.buffer_fullness < DEBUG_THRESHOLD_POINTS)

/* dac_loop(dv)
*
* Main thread function for sending data to the DAC.
*/
static void* dac_loop(netherdream * d) {
	int res = 0;

	while (1) {
		/* Wait for us to have data */
		int state;
		while ((state = d->state) == ST_READY) {
			trace(d, "L: waiting\n");
			std::unique_lock<std::mutex> lock(d->mutex);
			d->loop_cond.wait(lock);
		}

		if (state != ST_RUNNING)
			break;

		struct buffer_item* b = &d->buffer[d->frame_buffer_read];
		int cap;
		int expected_used, expected_fullness;

		while (1) {
			res = 0;

			/* Estimate how much data has been consumed since the
			* last time we got an ACK. */
			long long time_diff = microseconds()
				- d->conn.dc_last_ack_time;

			expected_used = time_diff * b->pps / 1000000;

			if (d->conn.resp.dac_status.playback_state != 2)
				expected_used = 0;

			expected_fullness =
				d->conn.resp.dac_status.buffer_fullness
				+ d->conn.unacked_points - expected_used;

			/* Now, see how much data we should write. */
			cap = 1700 - expected_fullness;

			if (cap > MIN_SEND_POINTS)
				break;
			if (d->conn.resp.dac_status.playback_state != 2) {
				microsleep(1000);
				break;
			}

			/* Wait a little. */
			int diff = MIN_SEND_POINTS - cap;
			int wait_time = 500 + (1000000L * diff / b->pps);

			if (SHOULD_TRACE())
				trace(d, "L: st %d om %d; b %d + %d - %d = %d"
					" -> c %d, wait %d us\n",
					d->conn.resp.dac_status.playback_state,
					d->conn.pending_meta_acks,
					d->conn.resp.dac_status.buffer_fullness,
					d->conn.unacked_points, expected_used,
					expected_fullness, cap, wait_time);

			microsleep(wait_time);

			if ((res = dac_get_acks(d, 0)) < 0)
				break;
		}

		if (res < 0)
			break;

		/* How many points can we send? */
		int b_left = b->points - b->idx;

		if (cap > b_left)
			cap = b_left;
		if (cap > 80)
			cap = 80;

		if (SHOULD_TRACE())
			trace(d, "L: st %d om %d; b %d + %d - %d = %d"
				" -> write %d\n",
				d->conn.resp.dac_status.playback_state,
				d->conn.pending_meta_acks,
				d->conn.resp.dac_status.buffer_fullness,
				d->conn.unacked_points, expected_used,
				expected_fullness, cap);

		res = dac_send_data(d, b->data + b->idx, cap, b->pps);
		if (res < 0)
			break;

		{
			std::unique_lock<std::mutex> lock(d->mutex);

			/* What next? */
			b->idx += cap;

			if (b->idx < b->points) {
				/* There's more in this frame. */
				continue;
			}

			b->idx = 0;

			if (b->repeatcount > 1) {
				/* Play this frame again? */
				b->repeatcount--;
			}
			else if (d->frame_buffer_fullness > 1) {
				/* Move to the next frame */
				d->frame_buffer_fullness--;
				d->frame_buffer_read++;
				if (d->frame_buffer_read >= BUFFER_NFRAMES)
					d->frame_buffer_read = 0;
				d->loop_cond.notify_all();
			}
			else if (b->repeatcount >= 0) {
				/* Stop playing until we get a new frame. */
				trace(d, "L: returning to idle\n");
				d->state = ST_READY;
			}
			else {
				/* repeatcount is negative and there's no new frame,
				* so just play this one over again. */
			}
		}
	}

	trace(d, "L: Shutting down.\n");
	d->state = ST_SHUTDOWN;
	d->loop_cond.notify_all();
	return 0;
}

int etherdream_connect(struct netherdream* d) {
	trace(d, "L: Connecting.\n");

	// Initialize buffer
	d->frame_buffer_read = 0;
	d->frame_buffer_fullness = 0;
	memset(d->buffer, 0, sizeof(d->buffer));

	// Connect to the DAC
	if (dac_connect(d) < 0) {
		trace(d, "!! DAC connection failed.\n");
		return -1;
	}

	d->state = ST_READY;

	d->workerthread = std::thread([d]() {
		dac_loop(d);
		});

	trace(d, "Ready.\n");

	return 0;
}


int etherdream_is_connected(struct netherdream* d) {
	return d->state == ST_READY;
}


void etherdream_disconnect(struct netherdream* d) {
	trace(d, "L: Disconnecting.\n");

	d->mutex.lock();
	if (d->state == ST_READY)
		d->loop_cond.notify_all();
	d->state = ST_SHUTDOWN;
	d->mutex.unlock();

	d->workerthread.join();
#ifdef _MSC_VER
	closesocket(d->conn.dc_sock);
#else
	close(d->conn.dc_sock);
#endif
}

/* etherdream_get_id(d)
*
* Documented in etherdream.h.
*/
unsigned long etherdream_get_id(struct netherdream* d) {
	return d->dac_id;
}

/* etherdream_get_in_addr(d)
*
* Documented in etherdream.h.
*/
const struct in_addr* etherdream_get_in_addr(struct netherdream* d) {
	return &d->addr;
}

/* etherdream_write(d, pts, npts, pps, reps)
*
* Documented in etherdream.h.
*/
int etherdream_write(struct netherdream* d, const struct etherdream_point* pts,
	int npts, int pps, int reps) {

	/* Limit maximum frame size */
	if (npts > BUFFER_POINTS_PER_FRAME)
		npts = BUFFER_POINTS_PER_FRAME;

	/* Ignore 0-repeat frames */
	if (!reps)
		return 0;

	d->mutex.lock();

	/* If not ready for a new frame, bail */
	if (d->frame_buffer_fullness == BUFFER_NFRAMES) {
		d->mutex.unlock();
		trace(d, "M: NOT READY: %d points, %d reps\n", npts, reps);
		return -1;
	}

	struct buffer_item* next = &d->buffer[(d->frame_buffer_read
		+ d->frame_buffer_fullness) % BUFFER_NFRAMES];

	d->mutex.unlock();

	// trace(d, "M: Writing: %d points, %d reps, %d pps\n", npts, reps, pps);

	/* XXX: automatically pad out small frames */

	int i;
	for (i = 0; i < npts; i++) {
		next->data[i].x = pts[i].x;
		next->data[i].y = pts[i].y;
		next->data[i].r = pts[i].r;
		next->data[i].g = pts[i].g;
		next->data[i].b = pts[i].b;
		next->data[i].i = pts[i].i;
		next->data[i].u1 = pts[i].u1;
		next->data[i].u2 = pts[i].u2;
		next->data[i].control = 0;
	}

	next->pps = pps;
	next->repeatcount = reps;
	next->points = npts;

	/* Advance buffer and signal the writing thread if necessary */
	d->mutex.lock();
	d->frame_buffer_fullness++;
	if (d->state == ST_READY)
		d->loop_cond.notify_one();
	d->state = ST_RUNNING;
	d->mutex.unlock();

	return 0;
}

/* etherdream_is_ready(d)
*
* Documented in etherdream.h.
*/
int etherdream_is_ready(struct netherdream* d) {
	d->mutex.lock();
	int ready = (d->frame_buffer_fullness != BUFFER_NFRAMES);
	d->mutex.unlock();
	return ready;
}

/* etherdream_wait_for_ready(d)
*
* Documented in etherdream.h.
*/
int etherdream_wait_for_ready(struct netherdream* d) {
	//d->mutex.lock();
	while (d->frame_buffer_fullness == BUFFER_NFRAMES && d->state != ST_SHUTDOWN) {
		std::unique_lock<std::mutex> lock(d->mutex); 
		d->loop_cond.wait(lock);
	}
	int is_shutdown = (d->state == ST_SHUTDOWN);
	//d->mutex.unlock();

	if (is_shutdown) {
		return -1;
	}
	else {
		return 0;
	}
}

/* etherdream_stop(d)
*
* Documented in etherdream.h.
*/
int etherdream_stop(struct netherdream* d) {
	d->mutex.lock();
	if (d->state == ST_RUNNING)
		d->buffer[d->frame_buffer_read].repeatcount = 0;
	d->mutex.unlock();
	return 0;
}

/* watch_for_dacs(arg)
*
* Thread function for the broadcast monitor thread. This listens for UDP
* broadcasts from Ether Dream boards on the network and adds them to our list.
*/
static void* watch_for_dacs() {
	int sock = socket(AF_INET, SOCK_DGRAM, 0);
	if (sock < 0) {
		log_socket_error(NULL, "socket");
		return NULL;
	}

	int opt = 1;
	if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt,
		sizeof opt) < 0) {
		log_socket_error(NULL, "setsockopt SO_REUSEADDR");
		return NULL;
	}

	struct sockaddr_in addr;
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	addr.sin_port = htons(7654);

	if (bind(sock, (struct sockaddr*)&addr, sizeof addr) < 0) {
		log_socket_error(NULL, "bind");
		return NULL;
	}

	trace(NULL, "_: listening for DACs...\n");

	while (1) {
		struct sockaddr_in src;
		struct dac_broadcast buf;
#ifdef _MSC_VER
		int srclen = sizeof src;
#else
		unsigned int srclen = sizeof src;
#endif
		int len = recvfrom(sock, (char*)&buf, sizeof buf, 0,
			(struct sockaddr*)&src, &srclen);
		if (len < 0) {
			log_socket_error(NULL, "recvfrom");
			return NULL;
		}

		/* See if this is a DAC we already knew about */
		dac_list_lock.lock();
		struct netherdream* p = dac_list;
		while (p) {
			if (p->addr.s_addr == src.sin_addr.s_addr)
				break;
			p = p->next;
		}

		if (p && (p->addr.s_addr == src.sin_addr.s_addr)) {
			dac_list_lock.unlock();
			continue;
		}

		dac_list_lock.unlock();

		/* Make a new DAC entry */
		struct netherdream* new_dac;
		new_dac = new netherdream();

		new_dac->addr = src.sin_addr;
		memcpy(new_dac->mac_address, buf.mac_address, 6);
		new_dac->dac_id = (buf.mac_address[3] << 16)
			| (buf.mac_address[4] << 8)
			| buf.mac_address[5];
		new_dac->sw_revision = buf.sw_revision;
		new_dac->state = ST_DISCONNECTED;

		trace(NULL, "_: Found new DAC: %s\n", inet_ntoa(src.sin_addr));

		dac_list_lock.lock();
		new_dac->next = dac_list;
		dac_list = new_dac;
		dac_list_lock.unlock();
	}

	trace(NULL, "_: Exiting\n");
	return NULL;
}

/* etherdream_lib_start()
*
* Documented in etherdream.h.
*/
int etherdream_lib_start(void) {

	// Set up the logging fd (just stderr for now)
	trace_fp = stderr;
	fprintf(trace_fp, "----------\n");
	fflush(trace_fp);
	trace(NULL, "== libetherdream started ==\n");


#ifdef _MSC_VER
	{
		WORD wVersionRequested;
		WSADATA wsaData;
		wVersionRequested = MAKEWORD(2, 2);
		auto err = WSAStartup(wVersionRequested, &wsaData);
		if (err != 0) {
			printf("WSAStartup failed with error: %d\n", err);
			return err;
		}
	}
#endif

	watcherThread = std::thread(watch_for_dacs);

	return 0;
}

/* etherdream_dac_count()
*
* Documented in etherdream.h.
*/
int etherdream_dac_count(void) {
	dac_list_lock.lock();

	int count = 0;
	struct netherdream* d = dac_list;
	while (d) {
		d = d->next;
		count++;
	}

	dac_list_lock.unlock();
	trace(NULL, "== etherdream_lib_get_dac_count(): %d\n", count);
	return count;
}

/* etherdream_get()
*
* Documented in etherdream.h.
*/
struct netherdream* etherdream_get(unsigned long idx) {
	struct netherdream* d = dac_list;
	unsigned long i = 0;

	while (d) {
		// Match by either numerical position or ID
		if (idx == i || idx == d->dac_id)
			return d;
		i++;
		d = d->next;
	}

	return NULL;
}