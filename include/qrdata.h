/** \file qrdata.h
 \brief function definitions for Qremote's handling of message bodies
 */
#ifndef QRDATA_H
#define QRDATA_H

#include <sys/types.h>

extern const char *successmsg[];

extern unsigned int need_recode(const char *, off_t);
extern void send_data(void);
extern void send_bdat(void);

extern const char *msgdata;
extern off_t msgsize;
extern int ascii;

#endif
