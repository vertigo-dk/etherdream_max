Etherdream driver for MAX/MSP + an example program.
It freezes the socket after 5 minutes in netherdream.cpp line 96: int res = select(d->conn.dc_sock + 1, (writable ? NULL : &set), (writable ? &set : NULL), &set, &t);
