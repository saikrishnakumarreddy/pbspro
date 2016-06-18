/*
 * Copyright (C) 1994-2016 Altair Engineering, Inc.
 * For more information, contact Altair at www.altair.com.
 *  
 * This file is part of the PBS Professional ("PBS Pro") software.
 * 
 * Open Source License Information:
 *  
 * PBS Pro is free software. You can redistribute it and/or modify it under the
 * terms of the GNU Affero General Public License as published by the Free 
 * Software Foundation, either version 3 of the License, or (at your option) any 
 * later version.
 *  
 * PBS Pro is distributed in the hope that it will be useful, but WITHOUT ANY 
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
 * PARTICULAR PURPOSE.  See the GNU Affero General Public License for more details.
 *  
 * You should have received a copy of the GNU Affero General Public License along 
 * with this program.  If not, see <http://www.gnu.org/licenses/>.
 *  
 * Commercial License Information: 
 * 
 * The PBS Pro software is licensed under the terms of the GNU Affero General 
 * Public License agreement ("AGPL"), except where a separate commercial license 
 * agreement for PBS Pro version 14 or later has been executed in writing with Altair.
 *  
 * Altair’s dual-license business model allows companies, individuals, and 
 * organizations to create proprietary derivative works of PBS Pro and distribute 
 * them - whether embedded or bundled with other software - under a commercial 
 * license agreement.
 * 
 * Use of Altair’s trademarks, including but not limited to "PBS™", 
 * "PBS Professional®", and "PBS Pro™" and Altair’s logos is subject to Altair's 
 * trademark licensing policies.
 *
 */
/**
 * @file    svr_mail.c
 *
 * @brief
 * 		svr_mail.c - send mail to mail list or owner of job on
 *		job begin, job end, and/or job abort
 *
 * 	Included public functions are:
 *		create_socket_and_connect()
 *		read_smtp_reply()
 *		write3_smtp_data()
 *		send_mail()
 *		send_mail_detach()
 *		svr_mailowner_id()
 *		svr_mailowner()
 *		svr_mailownerResv()
 *
 */

#include <pbs_config.h>   /* the master config generated by configure */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include "pbs_ifl.h"
#include "list_link.h"
#include "attribute.h"
#include "server_limits.h"
#include "log.h"

#ifdef WIN32
#include <windows.h>
#include <process.h>
#include "win.h"
#endif

#include "job.h"
#include "reservation.h"
#include "server.h"
#include "rpp.h"


/* External Functions Called */

extern void net_close(int);

/* Globol Data */

extern struct server server;
extern char *msg_job_abort;
extern char *msg_job_start;
extern char *msg_job_end;
extern char *msg_resv_abort;
extern char *msg_resv_start;
extern char *msg_resv_end;
extern char *msg_resv_confirm;
extern char *msg_job_stageinfail;

#ifdef WIN32

struct mail_param {
	int		type;
	char	*mailfrom;
	char	*mailto;
	char	*jobid;
	int		mailpoint;
	char	*jobname;
	char	*text;
};

/**
 * @brief
 *  	A thread safe way to connect to hostaddr at port
 *
 * @param[in]	host	-	destination host machine
 * @param[in]	port	-	port number
 *
 * @return	int
 * @retval	0	: success
 * @retval	-1	: error and errno will be set.
 */
int
create_socket_and_connect(char *host, unsigned int port)
{
	struct sockaddr_in remote;
	int		 sock;

	int		ret;
	int		non_block = 1;
	fd_set	writeset;
	struct	timeval tv;
	struct in_addr  haddr;
	unsigned long hostaddr;
	struct hostent *hp;

	hp = gethostbyname(host);
	if (hp == (struct hostent *)0) {
		errno = WSAGetLastError();
		return (-1);
	}

	memcpy((void *)&haddr, (void *)hp->h_addr_list[0], hp->h_length);
	hostaddr = ntohl(haddr.s_addr);

	/* get socket					*/

	sock = socket(AF_INET, SOCK_STREAM, 0);
	if (sock < 0) {
		errno = WSAGetLastError();
		return (-1);
	}
	/*	If local privilege port requested, bind to one	*/
	/*	Must be root privileged to do this		*/

	/*	connect to specified server host and port	*/

	remote.sin_addr.s_addr = htonl(hostaddr);
	remote.sin_port = htons((unsigned short)port);
	remote.sin_family = AF_INET;

	/* force socket to be non-blocking so we can timeout wait on it */

	if (ioctlsocket(sock, FIONBIO, &non_block) == SOCKET_ERROR) {
		errno = WSAGetLastError();
		return (-1);
	}
	FD_ZERO(&writeset);
	FD_SET((unsigned int)sock, &writeset);
	tv.tv_sec = 20;		/* connect timeout */
	tv.tv_usec = 0;

	if (connect(sock, (struct sockaddr *)&remote, sizeof(remote)) < 0) {
		errno = WSAGetLastError();
		switch (errno) {
			case WSAEINTR:
			case WSAEADDRINUSE:
			case WSAETIMEDOUT:
			case WSAECONNREFUSED:
				closesocket(sock);
				return (-1);
			case WSAEWOULDBLOCK:
				ret = select(1, NULL, &writeset, NULL, &tv);
				if (ret <= 0) {
					return (-1);
				}
				/* reset to blocking */
				non_block = 0;
				if (ioctlsocket(sock, FIONBIO, &non_block) == SOCKET_ERROR) {
					errno = WSAGetLastError();
					return (-1);
				}
				return (sock);
			default:
				closesocket(sock);
				return (-1);
		}

	} else {
		return (sock);
	}
}

/**
 * @brief
 * 		returns the reply code obtained from sock, in an SMTP protocol.
 *
 * @param[in]	sock	-	socket to receive the reply.
 *
 * @return	error code
 * @retval	554	: default - Transaction failed!
 *
 */
static int
read_smtp_reply(int sock)
{
	char	buf[512];
	int		got;
	int		ret = 554;	/* default is Transaction failed! */

	got = recv(sock, buf, sizeof(buf)-1, 0);
	if (got > 0) {
		buf[got] = '\0';
		sscanf(buf, "%d", &ret);
	}
	return (ret);
}

/**
 * @brief
 * 		write3_smtp_data - sends up to 3 messages down 'sock'. Specify NULL for msg* if none.
 *
 * @param[in]	sock	-	socket to send message
 * @param[in]	msg1	-	message 1
 * @param[in]	msg2	-	message 2
 * @param[in]	msg3	-	message 3
 *
 * @return	the number of bytes sent.
 * @retval	-1	: error
 */
static int
write3_smtp_data(int sock, char *msg1, char *msg2, char *msg3)
{
	int	sent;
	int	ct = 0;

	if (msg1) {
		if ((sent=send(sock, msg1, strlen(msg1), 0)) == SOCKET_ERROR) {
			return (SOCKET_ERROR);
		}
		ct += sent;
	}

	if (msg2) {
		if ((sent=send(sock, msg2, strlen(msg2), 0)) == SOCKET_ERROR) {
			return (SOCKET_ERROR);
		}
		ct += sent;
	}

	if (msg3) {
		if ((sent=send(sock, msg3, strlen(msg3), 0)) == SOCKET_ERROR) {
			return (SOCKET_ERROR);
		}
		ct += sent;
	}
	return (ct);
}
/**
 * @brief
 * 		A generic mail function to send mail.
 *
 * @param[in]	pv	-	mail parameters.
 *
 * @return - none
 */
unsigned __stdcall
send_mail(void *pv)
{
	struct	mail_param *m = (struct mail_param *)pv;
	int		type = m->type;
	char	*mailfrom = m->mailfrom;
	char	*mailto	= m->mailto;
	char	*jobid = m->jobid;
	int		mailpoint = m->mailpoint;
	char	*jobname = m->jobname;
	char	*text = m->text;
	char	mailhost[PBS_MAXHOSTNAME+1];
	char	mailto_full[PBS_MAXUSER+PBS_MAXHOSTNAME+3] = {0};	/* +3 for '<' ,'>' and Null char */
	char	mailfrom_full[PBS_MAXUSER+PBS_MAXHOSTNAME+3] = {0}; /* +3 for '<' ,'>' and Null char */
	int	sock;
	int	sent = 0;
	char	*stdmessage = (char *)0;
	char	*pc = NULL;
	char	*pc2 = NULL;
	int	reply;
	int	err_reply;
	int	err_write;
	extern  char server_host[];
	
	if (strchr(mailfrom, (int)'@')) { /* domain required */
		sprintf(mailfrom_full, "<%s>", mailfrom);
	} else {
		sprintf(mailfrom_full, "<%s@pbspro.com>", mailfrom);
	}
	
	pc = strtok(mailto, " ");
	while (pc) {

		if ((pc2=strchr(pc, (int)'@'))) {
			pc2++;
			strncpy(mailhost, pc2, (sizeof(mailhost) - 1));
		} else {
			strcpy(mailhost, "localhost");
		}

		if(pbs_conf.pbs_smtp_server_name != NULL) {
			sock = create_socket_and_connect(pbs_conf.pbs_smtp_server_name, 25);
		} else {
			sock = create_socket_and_connect(mailhost, 25);
		}

		if (sock < 0) {
			log_err(errno,"send_mail","Socket creation and connection Failed.");
			goto error;
		}
		
		err_reply = read_smtp_reply(sock);
		if (err_reply != 220) {		/* service not ready */
			log_err(err_reply,"send_mail","Service not ready for creation and connection of socket.");
			goto error;
		}
		
		err_write = write3_smtp_data(sock, "HELO ", mailhost, "\r\n");
		if (err_write == SOCKET_ERROR) {
			log_err(err_write,"send_mail","Conversation with the mail server cannot be initiated.");
			goto error;
		}
		
		err_reply = read_smtp_reply(sock);
		if (err_reply != 250) {
			log_err(err_reply,"send_mail","Service not ready for Initiation.");
			goto error;
		}
		
		err_write = write3_smtp_data(sock, "MAIL FROM: ", mailfrom_full, "\r\n");
		if (err_write == SOCKET_ERROR) {
			log_err(err_write,"send_mail","Error sending MAIL FROM: command to SMTP server");
			goto error;
		}
		
		err_reply = read_smtp_reply(sock);
		if (err_reply != 250) {
			log_err(err_reply,"send_mail","Service not ready for setting the MAIL FROM attribute");
			goto error;
		}
		
		sprintf(mailto_full, "<%s>", pc);
		err_write = write3_smtp_data(sock, "RCPT TO: ", mailto_full, "\r\n");
		if (err_write == SOCKET_ERROR) {
			log_err(err_write,"send_mail","Error sending RCPT TO: command to SMTP server");
			goto error;
		}
		
		err_reply = read_smtp_reply(sock);
		if (err_reply != 250) {
			log_err(err_reply,"send_mail","Service not ready for setting the RCPT TO attribute");
			goto error;
		}

		err_write = write3_smtp_data(sock, "DATA ", "\r\n", NULL);
		if (err_write == SOCKET_ERROR) {
			log_err(err_write,"send_mail","Error sending DATA command to SMTP server");
			goto error;
		}
		
		err_reply = read_smtp_reply(sock);
		if (err_reply != 354) {
			log_err(err_reply,"send_mail","Service Not Ready for Data Setting");
			goto error;
		}
		
		err_write = write3_smtp_data(sock, "To: ", pc, "\r\n");
		if (err_write == SOCKET_ERROR) {
			log_err(err_write,"send_mail","Error sending To: command to SMTP server");
			goto error;
		}

		if (type == 1) {
			err_write = write3_smtp_data(sock, "Subject: PBS RESERVATION ", jobid, "\n\r\n");
			if (err_write == SOCKET_ERROR) {
				log_err(err_write,"send_mail","Error sending PBS RESERVATION to SMTP server");
				goto error;
			}
			err_write = write3_smtp_data(sock, "Subject: PBS Reservation Id: ", jobid, "\r\n");
			if (err_write == SOCKET_ERROR) {
				log_err(err_write,"send_mail","Error sending PBS RESERVATION Id to SMTP server");
				goto error;
			}
			err_write = write3_smtp_data(sock, "Reservation Name: ", jobname, "\r\n");
			if (err_write == SOCKET_ERROR) {
				log_err(err_write,"send_mail","Error sending PBS Reservation Name to SMTP server");
				goto error;
			}
		} else if (type == 2) {
			err_write = write3_smtp_data(sock, "Subject: PBS Server on ", server_host, "\n\r\n");
			if (err_write == SOCKET_ERROR) {
				log_err(err_write,"send_mail","Error sending PBS Server name to SMTP server");
				goto error;
			}
		} else {
			err_write = write3_smtp_data(sock, "Subject: PBS JOB ", jobid, "\n\r\n");
			if (err_write == SOCKET_ERROR) {
				log_err(err_write,"send_mail","Error sending PBS JOB to SMTP server");
				goto error;
			}
			err_write = write3_smtp_data(sock, "Subject: PBS Job Id: ", jobid, "\r\n");
			if (err_write == SOCKET_ERROR) {
				log_err(err_write,"send_mail","Error sending PBS JOB Id to SMTP server");
				goto error;
			}
			err_write = write3_smtp_data(sock, "Job Name: ", jobname, "\r\n");
			if (err_write == SOCKET_ERROR) {
				log_err(err_write,"send_mail","Error sending PBS JOB Name to SMTP server");
				goto error;
			}
		}


		/* Now pipe in "standard" message */

		switch (mailpoint) {

			case MAIL_ABORT:
				stdmessage = msg_job_abort;
				break;

			case MAIL_BEGIN:
				stdmessage = msg_job_start;
				break;

			case MAIL_END:
				stdmessage = msg_job_end;
				break;

			case MAIL_STAGEIN:
				stdmessage = msg_job_stageinfail;
				break;

		}

		if (stdmessage) {
			err_write = write3_smtp_data(sock, stdmessage, "\r\n", NULL);
			if (err_write == SOCKET_ERROR) {
				log_err(err_write,"send_mail","Error sending Mail Point to SMTP server");
				goto error;
			}
		}

		if (text != (char *)0) {
			err_write = write3_smtp_data(sock, text, "\r\n", NULL); 
			if (err_write == SOCKET_ERROR) {
				log_err(err_write,"send_mail","Error sending Mail Data to SMTP server");
				goto error;
			}
		}

		err_write = write3_smtp_data(sock, ".\r\n", NULL, NULL);
		if (err_write == SOCKET_ERROR) {
			log_err(err_write,"send_mail","Error sending Mail Data Termination to SMTP server");
			goto error;
		}
		
		err_reply = read_smtp_reply(sock);
		if (err_reply != 250) {
			log_err(err_reply,"send_mail","Service not ready to terminate Mail Data");
			goto error;
		}
		
		err_write = write3_smtp_data(sock, "QUIT\r\n", NULL, NULL);
		if (err_write == SOCKET_ERROR) {
			log_err(err_write,"send_mail","Error sending QUIT to SMTP server");
			goto error;
		}
		
		err_reply = read_smtp_reply(sock);
		if (err_reply != 221) {
			log_err(err_reply,"send_mail","Service not ready to Quit");
			goto error;
		}
		error:		if (sock >= 0) {
			closesocket(sock);
		}
		pc = strtok(NULL, " ");
	}

	if (m->mailfrom)(void)free(m->mailfrom);
	if (m->mailto)(void)free(m->mailto);
	if (m->jobid)(void)free(m->jobid);
	if (m->jobname)(void)free(m->jobname);
	if (m->text)(void)free(m->text);
	(void)free(m);
	return (0);
}
/**
 * @brief
 * 		Send mail to owner of a job when an event happens that
 *		requires mail, such as the job starts, ends or is aborted.
 *		The event is matched against those requested by the user.
 *		For Unix/Linux, a child is forked to not hold up the Server.  This child
 *		will fork/exec sendmail and pipe the To, Subject and body to it.
 *
 * @param[in] type	-	0=JOB, 1=RESERVATION, 2=SERVER
 * @param[in] mailfrom	-	sender of the mail
 * @param[in] mailto	-	receiver of the mail
 * @param[in] jobid	-	job id
 * @param[in] mailpoint	-	which mail event is triggering the send
 * @param[in] jobname	-	Job Name
 * @param[in] text	-	the body text of the mail message
 *
 * @return - none
 */
void
send_mail_detach(int type, char *mailfrom, char *mailto, char *jobid, int mailpoint, char *jobname, char *text)
{
	DWORD	dwTID;
	HANDLE	h;

	struct mail_param *pmp = (struct mail_param *)malloc(sizeof(struct mail_param));

	if (pmp == (struct mail_param *)0)
		return;
	else
		memset((void *)pmp, 0, sizeof(struct mail_param));
	pmp->type = type;
	if ((pmp->mailfrom = strdup((mailfrom?mailfrom:""))) == NULL)
		goto err;
	if ((pmp->mailto = strdup((mailto?mailto:""))) == NULL)
		goto err;
	if ((pmp->jobid = strdup((jobid?jobid:""))) == NULL)
		goto err;
	pmp->mailpoint = mailpoint;
	if ((pmp->jobname = strdup((jobname?jobname:""))) == NULL)
		goto err;
	if ((pmp->text = strdup((text?text:""))) == NULL)
		goto err;

	h = (HANDLE) _beginthreadex(0, 0,  send_mail, pmp, 0, &dwTID);
	CloseHandle(h);
	return;
err:
	if (pmp->mailfrom)
		free(pmp->mailfrom);
	if (pmp->mailto)
		free(pmp->mailto);
	if (pmp->jobid)
		free(pmp->jobid);
	if (pmp->jobname)
		free(pmp->jobname);
	if (pmp->text)
		free(pmp->text);
	if (pmp)
		free(pmp);
	return;
}
#endif	/* WIN32 */

#define MAIL_ADDR_BUF_LEN 1024
/**
 * @brief
 * 		Send mail to owner of a job when an event happens that
 *		requires mail, such as the job starts, ends or is aborted.
 *		The event is matched against those requested by the user.
 *		For Unix/Linux, a child is forked to not hold up the Server.  This child
 *		will fork/exec sendmail and pipe the To, Subject and body to it.
 *
 * @param[in]	jid	-	the Job ID (string)
 * @param[in]	pjob	-	pointer to the job structure
 * @param[in]	mailpoint	-	which mail event is triggering the send
 * @param[in]	force	-	if non-zero, force the mail even if not requested
 * @param[in]	text	-	the body text of the mail message
 *
 * @return	none
 */
void
svr_mailowner_id(char *jid, job *pjob, int mailpoint, int force, char *text)
{
	int	 addmailhost;
	int	 i;
	char	*mailfrom;
	char	 mailto[MAIL_ADDR_BUF_LEN];
	int	 mailaddrlen = 0;
	struct array_strings *pas;
	char	*stdmessage = (char *)0;
	char	*pat;
	extern  char server_host[];

#ifndef WIN32
	FILE   *outmail;
	char   *margs[5];
	int     mfds[2];
	pid_t   mcpid;
#endif

	/* if force is true, force the mail out regardless of mailpoint */

	if (force != MAIL_FORCE) {
		if (pjob != 0) {

			/* see if user specified mail of this type */

			if (pjob->ji_wattr[(int)JOB_ATR_mailpnts].at_flags &ATR_VFLAG_SET) {
				if (strchr(pjob->ji_wattr[(int)JOB_ATR_mailpnts].at_val.at_str,
					mailpoint) == (char *)0)
					return;
			} else if (mailpoint != MAIL_ABORT)	/* not set, default to abort */
				return;

		} else if ((server.sv_attr[(int)SRV_ATR_mailfrom].at_flags & ATR_VFLAG_SET) == 0) {

			/* not job related, must be system related;  not sent unless */
			/* forced or if "mailfrom" attribute set         		 */
			return;
		}
	}

	/*
	 * ok, now we will fork a process to do the mailing to not
	 * hold up the server's other work.
	 */

#ifndef WIN32
	if (fork())
		return;		/* its all up to the child now */

	/*
	 * From here on, we are a child process of the server.
	 * Fix up file descriptors and signal handlers.
	 */

	if(pfn_rpp_terminate)
		rpp_terminate();
	net_close(-1);
	/* Unprotect child from being killed by kernel */
	daemon_protect(0, PBS_DAEMON_PROTECT_OFF);

#endif	/* ! WIN32 */

	/* Who is mail from, if SVR_ATR_mailfrom not set use default */

	if ((mailfrom = server.sv_attr[(int)SRV_ATR_mailfrom].at_val.at_str)==0)
		mailfrom = PBS_DEFAULT_MAIL;

	/* Who does the mail go to?  If mail-list, them; else owner */

	*mailto = '\0';
	if (pjob != 0) {
		if (jid == NULL)
			jid = pjob->ji_qs.ji_jobid;

		if (pjob->ji_wattr[(int)JOB_ATR_mailuser].at_flags & ATR_VFLAG_SET) {

			/* has mail user list, send to them rather than owner */

			pas = pjob->ji_wattr[(int)JOB_ATR_mailuser].at_val.at_arst;
			if (pas != (struct array_strings *)0) {
				for (i = 0; i < pas->as_usedptr; i++) {
					addmailhost = 0;
					mailaddrlen += strlen(pas->as_string[i]) + 2;
					if ((pbs_conf.pbs_mail_host_name)  && 
					    (strchr(pas->as_string[i], (int)'@') == NULL)) {
							/* no host specified in address and      */
							/* pbs_mail_host_name is defined, use it */
							mailaddrlen += strlen(pbs_conf.pbs_mail_host_name) + 1;
							addmailhost = 1;
					}
					if (mailaddrlen < sizeof(mailto)) {
						(void)strcat(mailto, pas->as_string[i]);
						if (addmailhost) {
							/* append pbs_mail_host_name */
							(void)strcat(mailto, "@");
							(void)strcat(mailto, pbs_conf.pbs_mail_host_name);
						}
						(void)strcat(mailto, " ");
					} else {
					  	sprintf(log_buffer,"Email list is too long: \"%.77s...\"", mailto);
						log_event(PBSEVENT_JOB, PBS_EVENTCLASS_JOB, LOG_WARNING, pjob->ji_qs.ji_jobid, log_buffer);
						break;
					}						
				}
			}

		} else {

			/* no mail user list, just send to owner */

			(void)strncpy(mailto, pjob->ji_wattr[(int)JOB_ATR_job_owner].at_val.at_str, sizeof(mailto));
			mailto[(sizeof(mailto) - 1)] = '\0';
			/* if pbs_mail_host_name is set in pbs.conf, then replace the */
			/* host name with the name specified in pbs_mail_host_name    */ 
			if (pbs_conf.pbs_mail_host_name) {
				if ((pat = strchr(mailto, (int)'@')) != NULL) 
					*pat = '\0';	/* remove existing @host */
				if ((strlen(mailto) + strlen(pbs_conf.pbs_mail_host_name) + 1) < sizeof(mailto)) {
					/* append the pbs_mail_host_name since it fits */
					strcat(mailto, "@");
					strcat(mailto, pbs_conf.pbs_mail_host_name);
				} else {					
				  	if (pat)
						*pat = '@';	/* did't fit, restore the "at" sign */
				  	sprintf(log_buffer,"Email address is too long: \"%.77s...\"", mailto);
					log_event(PBSEVENT_JOB, PBS_EVENTCLASS_JOB, LOG_WARNING, pjob->ji_qs.ji_jobid, log_buffer);
				}						
			}
		}

	} else {
		/* send system related mail to "mailfrom" */
		strcpy(mailto, mailfrom);
	}

#ifdef WIN32
	/* if pjob is not null, then send a JOB type email (1st param=0); */
	/* otherwise, send a SERVER type email (1st param=2)               */

	send_mail_detach((pjob?0:2), mailfrom, mailto,
		(pjob?pjob->ji_qs.ji_jobid:""), mailpoint,
		(pjob?pjob->ji_wattr[(int)JOB_ATR_jobname].at_val.at_str:""),
		text);

#else
	/* setup sendmail command line with -f from_whom */

	margs[0] = SENDMAIL_CMD;
	margs[1] = "-f";
	margs[2] = mailfrom;
	margs[3] = mailto;
	margs[4] = NULL;

	if (pipe(mfds) == -1)
		exit(1);

	mcpid = fork();
	if(mcpid == 0) {
		/* this child will be sendmail with its stdin set to the pipe */
      if (mfds[0] != 0) {
            (void)close(0);
            if (dup(mfds[0]) == -1) 
				exit(1);
        }
        (void)close(1);
        (void)close(2);
        if (execv(SENDMAIL_CMD, margs) == -1)
			exit(1);
	}

	/* parent (not the real server though) will write body of message on pipe */
	(void)close(mfds[0]);
	outmail = fdopen(mfds[1], "w");
	if (outmail == NULL)
		exit(1);

	/* Pipe in mail headers: To: and Subject: */

	fprintf(outmail, "To: %s\n", mailto);

	if (pjob)
		fprintf(outmail, "Subject: PBS JOB %s\n\n", jid);
	else
		fprintf(outmail, "Subject: PBS Server on %s\n\n", server_host);

	/* Now pipe in "standard" message */

	switch (mailpoint) {

		case MAIL_ABORT:
			stdmessage = msg_job_abort;
			break;

		case MAIL_BEGIN:
			stdmessage = msg_job_start;
			break;

		case MAIL_END:
			stdmessage = msg_job_end;
			break;

		case MAIL_STAGEIN:
			stdmessage = msg_job_stageinfail;
			break;

	}

	if (pjob) {
		fprintf(outmail, "PBS Job Id: %s\n", jid);
		fprintf(outmail, "Job Name:   %s\n",
			pjob->ji_wattr[(int)JOB_ATR_jobname].at_val.at_str);
	}
	if (stdmessage)
		fprintf(outmail, "%s\n", stdmessage);
	if (text != (char *)0)
		fprintf(outmail, "%s\n", text);
	fclose(outmail);

	exit(0);
#endif	/* WIN32 */
}
/**
 * @brief
 * 		svr_mailowner - Send mail to owner of a job when an event happens that
 *		requires mail, such as the job starts, ends or is aborted.
 *		The event is matched against those requested by the user.
 *		For Unix/Linux, a child is forked to not hold up the Server.  This child
 *		will fork/exec sendmail and pipe the To, Subject and body to it.
 *
 * @param[in]	pjob	-	ptr to job (null for server based mail)
 * @param[in]	mailpoint	-	note, single character
 * @param[in]	force	-	if set, force mail delivery
 * @param[in]	text	-	additional message text
 */
void
svr_mailowner(job *pjob, int mailpoint, int force, char *text)
{
	svr_mailowner_id((char *)0, pjob, mailpoint, force, text);
}

/**
 * @brief
 * 		Send mail to owner of a reservation when an event happens that
 *		requires mail, such as the reservation starts, ends or is aborted.
 *		The event is matched against those requested by the user.
 *		For Unix/Linux, a child is forked to not hold up the Server.  This child
 *		will fork/exec sendmail and pipe the To, Subject and body to it.
 *
 * @param[in]	presv	-	pointer to the reservation structure
 * @param[in]	mailpoint	-	which mail event is triggering the send
 * @param[in]	force	-	if non-zero, force the mail even if not requested
 * @param[in]	text	-	the body text of the mail message
 *
 * @return	none
 */
void
svr_mailownerResv(resc_resv *presv, int mailpoint, int force, char *text)
{
	int	 i;
	int	 addmailhost;
	char	*mailfrom;
	char	 mailto[MAIL_ADDR_BUF_LEN];
	int	 mailaddrlen = 0;
	struct array_strings *pas;
	char	*pat;
	char	*stdmessage = (char *)0;
#ifndef WIN32
	FILE	*outmail;
	char	*margs[5];
	int	 mfds[2];
	pid_t	 mcpid;
#endif

	if (force != MAIL_FORCE) {
		/*Not forcing out mail regardless of mailpoint */

		if (presv->ri_wattr[(int)RESV_ATR_mailpnts].at_flags &ATR_VFLAG_SET) {
			/*user has set one or mode mailpoints is this one included?*/
			if (strchr(presv->ri_wattr[(int)RESV_ATR_mailpnts].at_val.at_str,
				mailpoint) == (char *)0)
				return;
		} else {
			/*user hasn't bothered to set any mailpoints so default to
			 *sending mail only in the case of reservation deletion and
			 *reservation confirmation
			 */
			if ((mailpoint != MAIL_ABORT) && (mailpoint != MAIL_CONFIRM))
				return;
		}
	}

	if (presv->ri_wattr[(int)RESV_ATR_mailpnts].at_flags &ATR_VFLAG_SET) {
		if (strchr(presv->ri_wattr[(int)RESV_ATR_mailpnts].at_val.at_str,
			MAIL_NONE) != (char *)0)
			return;
	}

	/*
	 * ok, now we will fork a process to do the mailing to not
	 * hold up the server's other work.
	 */

#ifndef WIN32
	if (fork())
		return;		/* its all up to the child now */

	/*
	 * From here on, we are a child process of the server.
	 * Fix up file descriptors and signal handlers.
	 */

	rpp_terminate();
	net_close(-1);

	/* Unprotect child from being killed by kernel */
	daemon_protect(0, PBS_DAEMON_PROTECT_OFF);

#endif	/* ! WIN32 */

	/* Who is mail from, if SVR_ATR_mailfrom not set use default */

	if ((mailfrom = server.sv_attr[(int)SRV_ATR_mailfrom].at_val.at_str)==0)
		mailfrom = PBS_DEFAULT_MAIL;

	/* Who does the mail go to?  If mail-list, them; else owner */

	*mailto = '\0';
	if (presv->ri_wattr[(int)RESV_ATR_mailuser].at_flags & ATR_VFLAG_SET) {

		/* has mail user list, send to them rather than owner */

		pas = presv->ri_wattr[(int)RESV_ATR_mailuser].at_val.at_arst;
		if (pas != (struct array_strings *)0) {
			for (i = 0; i < pas->as_usedptr; i++) {
				addmailhost = 0;
				mailaddrlen += strlen(pas->as_string[i]) + 2;
				if ((pbs_conf.pbs_mail_host_name)  && 
				    (strchr(pas->as_string[i], (int)'@') == NULL)) {
						/* no host specified in address and      */
						/* pbs_mail_host_name is defined, use it */
						mailaddrlen += strlen(pbs_conf.pbs_mail_host_name) + 1;
						addmailhost = 1;
				}
				if (mailaddrlen < sizeof(mailto)) {
					(void)strcat(mailto, pas->as_string[i]);
					if (addmailhost) {
						/* append pbs_mail_host_name */
						(void)strcat(mailto, "@");
						(void)strcat(mailto, pbs_conf.pbs_mail_host_name);
					} else {
					  	sprintf(log_buffer,"Email list is too long: \"%.77s...\"", mailto);
						log_event(PBSEVENT_JOB, PBS_EVENTCLASS_JOB, LOG_WARNING, presv->ri_qs.ri_resvID, log_buffer);
						break;
					}						
					(void)strcat(mailto, " ");
				}
			}
		}

	} else {

		/* no mail user list, just send to owner */

		(void)strncpy(mailto, presv->ri_wattr[(int)RESV_ATR_resv_owner].at_val.at_str, sizeof(mailto));
		mailto[(sizeof(mailto) - 1)] = '\0';
		/* if pbs_mail_host_name is set in pbs.conf, then replace the */
		/* host name with the name specified in pbs_mail_host_name    */ 
		if (pbs_conf.pbs_mail_host_name) {
			if ((pat = strchr(mailto, (int)'@')) != NULL) 
				*pat = '\0';	/* remove existing @host */
			if ((strlen(mailto) + strlen(pbs_conf.pbs_mail_host_name) + 1) < sizeof(mailto)) {
				/* append the pbs_mail_host_name since it fits */
				strcat(mailto, "@");
				strcat(mailto, pbs_conf.pbs_mail_host_name);
			} else {
				if (pat)
					*pat = '@';	/* did't fit, restore the "at" sign */
			  	sprintf(log_buffer,"Email address is too long: \"%.77s...\"", mailto);
				log_event(PBSEVENT_JOB, PBS_EVENTCLASS_JOB, LOG_WARNING, presv->ri_qs.ri_resvID, log_buffer);
			}
		}
	}

#ifdef WIN32
	send_mail_detach(1, mailfrom, mailto, presv->ri_qs.ri_resvID, mailpoint,
		presv->ri_wattr[(int)RESV_ATR_resv_name].at_val.at_str, text);
#else

	/* setup sendmail command line with -f from_whom */

	margs[0] = SENDMAIL_CMD;
	margs[1] = "-f";
	margs[2] = mailfrom;
	margs[3] = mailto;
	margs[4] = NULL;

	if (pipe(mfds) == -1)
		exit(1);

	mcpid = fork();
	if(mcpid == 0) {
		/* this child will be sendmail with its stdin set to the pipe */
      if (mfds[0] != 0) {
            (void)close(0);
            if (dup(mfds[0]) == -1) 
				exit(1);
        }
        (void)close(1);
        (void)close(2);
        if (execv(SENDMAIL_CMD, margs) == -1)
			exit(1);
	}

	/* parent (not the real server though) will write body of message on pipe */
	(void)close(mfds[0]);
	outmail = fdopen(mfds[1], "w");
	if (outmail == NULL)
		exit(1);

	/* Pipe in mail headers: To: and Subject: */

	fprintf(outmail, "To: %s\n", mailto);
	fprintf(outmail, "Subject: PBS RESERVATION %s\n\n", presv->ri_qs.ri_resvID);

	/* Now pipe in "standard" message */

	switch (mailpoint) {

		case MAIL_ABORT:
			/*"Aborted by Server, Scheduler, or User "*/
			stdmessage = msg_resv_abort;
			break;

		case MAIL_BEGIN:
			/*"Reservation period starting"*/
			stdmessage = msg_resv_start;
			break;

		case MAIL_END:
			/*"Reservation terminated"*/
			stdmessage = msg_resv_end;
			break;

		case MAIL_CONFIRM:
			/*scheduler requested, "CONFIRM reservation"*/
			stdmessage = msg_resv_confirm;
			break;
	}

	fprintf(outmail, "PBS Reservation Id: %s\n", presv->ri_qs.ri_resvID);
	fprintf(outmail, "Reservation Name:   %s\n",
		presv->ri_wattr[(int)RESV_ATR_resv_name].at_val.at_str);
	if (stdmessage)
		fprintf(outmail, "%s\n", stdmessage);
	if (text != (char *)0)
		fprintf(outmail, "%s\n", text);
	fclose(outmail);

	exit(0);
#endif	/* ! WIN32 */
}