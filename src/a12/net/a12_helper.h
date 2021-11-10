/*
 * Copyright: 2018-2020, Bjorn Stahl
 * License: 3-Clause BSD
 * Description: This is a set of helper functions to deal with the event loop
 * specifically for acting as a translation proxy between shmif- and a12. The
 * 'server' and 'client' naming is a misnomer as the 'server' act as a local
 * shmif server and remote a12 client, while the 'client' act as a local shmif
 * client and remote a12 server.
 */

#ifndef HAVE_A12_HELPER

enum a12helper_pollstate {
	A12HELPER_POLL_SHMIF = 1,
	A12HELPER_WRITE_OUT = 2,
	A12HELPER_DATA_IN = 4
};

struct a12helper_opts {
	struct a12_vframe_opts (*eval_vcodec)(
		struct a12_state* S, int segid, struct shmifsrv_vbuffer*, void* tag);
	void* tag;

/* Set to the maximum distance between acknowledged frame and pending outgoing
 * and halt releasing client vframe until that resets back to within tolerance.
 * This is a coarse congestion control mechanism, meant as a placeholder until
 * something more refined can be developed. */
	size_t vframe_block;

	int dirfd_temp;
	int dirfd_cache;

/* a12cl_shmifsrv- specific: set to a valid local connection-point and incoming
 * EXIT_ events will be translated to DEVICE_NODE events, preventing the remote
 * side from closing the window */
 	const char* redirect_exit;

/* a12cl_shmifsrv- specific: set to a valid local connection-point and it will
 * be set as the DEVICE_NODE alternate for incoming connections */
	const char* devicehint_cp;
};

/*
 * Take a prenegotiated connection [S] and an accepted shmif client [C] and
 * use [fd_in, fd_out] (which can be set to the same and treated as a socket)
 * as the bitstream carrer.
 *
 * This will block until the connection is terminated.
 */
void a12helper_a12cl_shmifsrv(struct a12_state* S,
	struct shmifsrv_client* C, int fd_in, int fd_out, struct a12helper_opts);

/*
 * Single threaded read/write until the context has been authenticated
 */
bool a12helper_wait_for_auth(struct a12_state* S, int fd_in, int fd_out);

/*
 * Take a prenegotiated connection [S] serialized over [fd_in/fd_out] and
 * map to connections accessible via the [cp] connection point.
 *
 * Returns:
 * a12helper_pollstate bitmap
 *
 * Error codes:
 *  -EINVAL : invalid connection point
 *  -ENOENT : couldn't make shmif connection
 */
int a12helper_a12srv_shmifcl(
	struct a12_state* S, const char* cp, int fd_in, int fd_out);

#endif
