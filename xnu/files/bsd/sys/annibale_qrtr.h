/*
 * QRTR as the QMI helpers (sys/annibale_qmi.h) need it: datagrams to and from a port of a node of
 * Qualcomm's IPC router, and the question "where is that service".
 *
 * On the phone the router's other nodes are the processors beside ours (the Wi-Fi DSP is one), and
 * the datagrams travel over GLINK; whoever implements that hands these four calls to the QMI
 * helpers with annibale_qmi_set_transport(). On a development machine a test puts a fake in the
 * same place. Nothing here knows what is inside a datagram.
 */
#ifndef _SYS_ANNIBALE_QRTR_H_
#define _SYS_ANNIBALE_QRTR_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct annibale_qrtr_port;

/*
 * A datagram for the port has arrived from (node, port). The data is the caller's again on return.
 */
typedef void (*annibale_qrtr_receive_t)(void *context, uint32_t node, uint32_t port, const void *data, size_t length);
/*
 * A server of a service that was asked for with lookup() has appeared at (node, port), or the one
 * that was there has gone. Called once for each that exists when the question is asked, too.
 */
typedef void (*annibale_qrtr_server_t)(void *context, bool arrived, uint32_t service, uint32_t version, uint32_t instance,
    uint32_t node, uint32_t port);

/*
 * Both callbacks come from one thread at a time, never from interrupt context, and may sleep
 * (the QMI helpers take a mutex in them and call their user's handlers from them). `context` is
 * what open() was given.
 */
struct annibale_qrtr_ops {
	/* A port of our own node; NULL if there is none to be had. */
	struct annibale_qrtr_port *(*open)(annibale_qrtr_receive_t receive, annibale_qrtr_server_t server, void *context);
	/* One datagram to (node, port); 0, or an errno. May sleep. Up to 8192 bytes. */
	int (*send)(struct annibale_qrtr_port *port, uint32_t node, uint32_t to_port, const void *data, size_t length);
	/* Tell this port's `server` callback about the servers of that service, now and from now on; 0, or an errno. */
	int (*lookup)(struct annibale_qrtr_port *port, uint32_t service, uint32_t version, uint32_t instance);
	/* No callback comes after this has returned. */
	void (*close)(struct annibale_qrtr_port *port);
};

/* The transport there is; NULL takes it away (handles made before keep the one they were made with). */
void annibale_qmi_set_transport(const struct annibale_qrtr_ops *ops);

/*
 * The router (bsd/dev/annibale_qrtr.c): the four calls above over whichever link is attached. The link
 * names the function that sends one whole datagram, header and all (NULL: none, the router's tables
 * emptied), and hands it each datagram it receives, and a moment to answer lookups, from its one thread.
 */
extern const struct annibale_qrtr_ops annibale_qrtr;
void annibale_qrtr_attach(int (*send)(const void *data, uint32_t length));
void annibale_qrtr_input(const void *data, uint32_t length);
void annibale_qrtr_fresh(void);

struct annibale_qrtr_counts {
	uint32_t peer;                          /* the other side's node, 0 before it has sent anything */
	uint32_t hellos, in, out, services;
};
void annibale_qrtr_counts(struct annibale_qrtr_counts *counts);

#endif /* _SYS_ANNIBALE_QRTR_H_ */
