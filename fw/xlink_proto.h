/*
 * machboot
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * The messages on the XA-XD link, for both sides: the RP2040's
 * (rp2040/src/msc.c) and machboot's (fw/xlink.c). docs/xlink.md.
 *
 * Shared by rp2040/src (which includes it from here) and fw/.
 *
 * Every message is the RP2040's: [op][tag][pc][0] and its arguments;
 * machboot replies [op|0x80][tag][status][n] and its data. machboot
 * asks for things by raising FLAGS bits, and the RP2040 comes for them.
 */

#ifndef XLINK_PROTO_H
#define XLINK_PROTO_H

#define XL_OP_HELLO	1
#define XL_OP_READ	2
#define XL_OP_WRITE	3
#define XL_OP_EJECT	4
#define XL_OP_START	5	/* machboot will pull PROGRAMN: let that reset through */
#define XL_OP_SET	6	/* the settings the RP2040 keeps, to machboot (text) */
#define XL_OP_GET	7	/* machboot's settings, to keep (text) */
#define XL_OP_REQ	8	/* what machboot asks for (FLAGS bit 3): reply [kind][0][0][0] + args */
#define XL_OP_CHUNK	9	/* [off 4][n 4]: n bytes of the file a PUT copies; reply status XL_R_* */
#define XL_OP_DONE	10	/* [kind][result][0][0] + data: the request's outcome */
#define XL_OP_NOTE	11	/* text for machboot to show (a system that did not start) */

#define XL_F_SHARED	0x01
#define XL_F_STARTING	0x02
#define XL_F_SETTINGS	0x04
#define XL_F_REQUEST	0x08

/* requests (REQ's reply) */
#define XL_RQ_LIST	1	/* DONE data: [n][0][0][0][free 4][largest 4][size 4] + n items */
#define XL_RQ_PUT	2	/* args [len 4][name 24][ver 16]; CHUNKs; DONE data [crc 4] */
#define XL_RQ_RUN	3	/* args [crc 4]: load that system into the FPGA (machboot goes) */
#define XL_RQ_DROP	4	/* args [crc 4] */

/* results, and CHUNK's status */
#define XL_R_OK		0
#define XL_R_NOROOM	1
#define XL_R_ERROR	2
#define XL_R_NOTFOUND	3
#define XL_R_CANCELLED	4
#define XL_R_NOLINK	5	/* (machboot's own: the RP2040 did not come) */

#define XL_CHUNK	1536	/* bytes per CHUNK */
#define XL_ITEM		52	/* a LIST item: [crc 4][len 4][off 4][name 24][ver 16] */

#endif
