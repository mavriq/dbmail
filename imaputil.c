/*
 Copyright (C) 1999-2004 IC & S  dbmail@ic-s.nl

 This program is free software; you can redistribute it and/or 
 modify it under the terms of the GNU General Public License 
 as published by the Free Software Foundation; either 
 version 2 of the License, or (at your option) any later 
 version.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with this program; if not, write to the Free Software
 Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
*/

/* $Id$
 * 
 * imaputil.c
 *
 * IMAP-server utility functions implementations
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <time.h>
#include <ctype.h>
#include <unistd.h>
#include <errno.h>
#include "dbmail.h"
#include "imaputil.h"
#include "imap4.h"
#include "debug.h"
#include "db.h"
#include "memblock.h"
#include "dbsearch.h"
#include "rfcmsg.h"

#ifndef MAX_LINESIZE
#define MAX_LINESIZE (10*1024)
#endif

#define BUFLEN 2048
#define SEND_BUF_SIZE 1024
#define MAX_ARGS 512

/* cache */
extern cache_t cached_msg;

/* consts */
const char AcceptedChars[] =
    "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"
    "!@#$%^&*()-=_+`~[]{}\\|'\" ;:,.<>/? \n\r";

const char AcceptedTagChars[] =
    "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"
    "!@#$%^&-=_`~\\|'\" ;:,.<>/? ";

const char AcceptedMailboxnameChars[] =
    "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"
    "-=/ _.&,+@()[]";

extern const char *month_desc[];



char base64encodestring[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/* returned by date_sql2imap() */
#define IMAP_STANDARD_DATE "03-Nov-1979 00:00:00 +0000"
char _imapdate[IMAP_INTERNALDATE_LEN] = IMAP_STANDARD_DATE;

/* returned by date_imap2sql() */
#define SQL_STANDARD_DATE "1979-11-03 00:00:00"
char _sqldate[SQL_INTERNALDATE_LEN + 1] = SQL_STANDARD_DATE;


const int month_len[] = {
	31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
};

const char *item_desc[] = {
	"TEXT", "HEADER", "MIME", "HEADER.FIELDS", "HEADER.FIELDS.NOT"
};

const char *envelope_items[] = {
	"from", "sender", "reply-to", "to", "cc", "bcc", NULL
};

/*
 *
 *
 *  Some basic string handling utilities
 *
 *
 *
 */
GString * g_list_join(GList * list, char * sep)
{
	GString *string = g_string_new("");
	if (sep == NULL)
		sep="";
	if (list == NULL)
		return string;
	list = g_list_first(list);
	string = g_string_append(string, (gchar *)list->data);
	while((list = g_list_next(list))) {
		string = g_string_append(string,sep);
		string = g_string_append(string,(gchar *)list->data);
	}
	return string;	
}

GList * g_string_split(GString * string, char * sep)
{
	GList * list = NULL;
	char **array = (char **)g_strsplit((const gchar *)string->str, (const gchar *)sep,0);
	int i, len = 0;
	while(array[len++]);
	len--;
	for (i=0; i<len; i++)
		list = g_list_append(list,g_strdup(array[i]));
	g_strfreev(array);
	return list;
}
/*
 * append a formatted GString to a GList
 */
GList * g_list_append_printf(GList * list, char * format, ...)
{
	char *str = (char *)my_malloc(sizeof(char) * BUFLEN);
	va_list argp;
	va_start(argp, format);
	vsnprintf(str, sizeof(char) * BUFLEN, format, argp);
	list = g_list_append(list, strdup(str));
	my_free(str);
	return list;
}

/* some basic imap type utils */

/*
 *  build a parentisized list (4.4) from a GList
 */
char *dbmail_imap_plist_as_string(GList * list)
{
	char *p;
	GString * tmp1 = g_string_new("");
	GString * tmp2 = g_list_join(list, " ");
	g_string_printf(tmp1,"(%s)", tmp2->str);
	p = tmp1->str;
	g_string_free(tmp1,FALSE);
	g_string_free(tmp2,TRUE);
	return p;
}
/* 
 * return a quoted or literal astring
 */

char *dbmail_imap_astring_as_string(const char *s)
{
	int i;
	char *p;
	GString * tmp = g_string_new("");
	g_string_printf(tmp, "\"%s\"", s); /* fallback to quoted "string" */
	for (i=0; s[i]; i++) { /* but check whether we must use literal string {octets}\r\nstring */
		if ( !(s[i] & 0xe0) || (s[i] & 0x80) || (s[i] == '"') || (s[i] == '\\')) 
			g_string_printf(tmp, "{%lu}\r\n%s", (unsigned long) strlen(s), s);
	}
	p = tmp->str;
	g_string_free(tmp,FALSE);
	return p;
}
/* 
 * retrieve_structure()
 *
 * retrieves the MIME-IMB structure of a message. The msg should be in the format
 * as build by db_fetch_headers().
 *
 * shows extension data if show_extension_data != 0
 *
 * returns -1 on error, 0 on success
 */
int retrieve_structure(FILE * outstream, mime_message_t * msg,
		       int show_extension_data)
{
	struct mime_record *mr;
	struct element *curr;
	struct list *header_to_use;
	mime_message_t rfcmsg;
	char *subtype, *extension, *newline;
	int is_mime_multipart = 0, is_rfc_multipart = 0;
	int rfc822 = 0;

	fprintf(outstream, "(");

	mime_findfield("content-type", &msg->mimeheader, &mr);
	is_mime_multipart = (mr
			     && strncasecmp(mr->value, "multipart",
					    strlen("multipart")) == 0
			     && !msg->message_has_errors);

	mime_findfield("content-type", &msg->rfcheader, &mr);
	is_rfc_multipart = (mr
			    && strncasecmp(mr->value, "multipart",
					   strlen("multipart")) == 0
			    && !msg->message_has_errors);

	/* eddy */
	if (mr
	    && strncasecmp(mr->value, "message/rfc822",
			   strlen("message/rfc822")) == 0) {
		rfc822 = 1;
	}


	if (rfc822 || (!is_rfc_multipart && !is_mime_multipart)) {
		/* show basic fields:
		 * content-type, content-subtype, (parameter list), 
		 * content-id, content-description, content-transfer-encoding,
		 * size
		 */

		if (msg->mimeheader.start == NULL)
			header_to_use = &msg->rfcheader;	/* we're dealing with a single-part RFC msg here */
		else
			header_to_use = &msg->mimeheader;	/* we're dealing with a pure-MIME header here */

		mime_findfield("content-type", header_to_use, &mr);
		if (mr && strlen(mr->value) > 0)
			show_mime_parameter_list(outstream, mr, 1, 0);
		else
			fprintf(outstream, "\"TEXT\" \"PLAIN\" (\"CHARSET\" \"US-ASCII\")");	/* default */

		mime_findfield("content-id", header_to_use, &mr);
		if (mr && strlen(mr->value) > 0) {
			fprintf(outstream, " ");
			quoted_string_out(outstream, mr->value);
		} else
			fprintf(outstream, " NIL");

		mime_findfield("content-description", header_to_use, &mr);
		if (mr && strlen(mr->value) > 0) {
			fprintf(outstream, " ");
			quoted_string_out(outstream, mr->value);
		} else
			fprintf(outstream, " NIL");

		mime_findfield("content-transfer-encoding", header_to_use,
			       &mr);
		if (mr && strlen(mr->value) > 0) {
			fprintf(outstream, " ");
			quoted_string_out(outstream, mr->value);
		} else
			fprintf(outstream, " \"7BIT\"");

		/* now output size */
		/* add msg->bodylines because \n is dumped as \r\n */
		if (msg->mimeheader.start && msg->rfcheader.start)
			fprintf(outstream, " %llu",
				msg->bodysize + msg->mimerfclines +
				msg->rfcheadersize - msg->rfcheaderlines);
		else
			fprintf(outstream, " %llu",
				msg->bodysize + msg->bodylines);


		/* now check special cases, first case: message/rfc822 */
		mime_findfield("content-type", header_to_use, &mr);
		if (mr
		    && strncasecmp(mr->value, "message/rfc822",
				   strlen("message/rfc822")) == 0
		    && header_to_use != &msg->rfcheader) {
			/* msg/rfc822 found; extra items to be displayed:
			 * (a) body envelope of rfc822 msg
			 * (b) body structure of rfc822 msg
			 * (c) msg size (lines)
			 */

			if (retrieve_envelope(outstream, &msg->rfcheader)
			    == -1)
				return -1;

			fprintf(outstream, " ");

			memmove(&rfcmsg, msg, sizeof(rfcmsg));
			rfcmsg.mimeheader.start = NULL;	/* forget MIME-part */
			
			if (retrieve_structure
			    (outstream, &rfcmsg,
			     show_extension_data) == -1)
				return -1;

			/* output # of lines */
			fprintf(outstream, " %llu", msg->bodylines);
		}
		/* now check second special case: text 
		 * NOTE: if 'content-type' is absent, TEXT is assumed 
		 */
		if ((mr
		     && strncasecmp(mr->value, "text",
				    strlen("text")) == 0) || !mr) {
			/* output # of lines */
			if (msg->mimeheader.start && msg->rfcheader.start)
				fprintf(outstream, " %llu",
					msg->mimerfclines);
			else
				fprintf(outstream, " %llu", msg->bodylines);
		}

		if (show_extension_data) {
			mime_findfield("content-md5", header_to_use, &mr);
			if (mr && strlen(mr->value) > 0) {
				fprintf(outstream, " ");
				quoted_string_out(outstream, mr->value);
			} else
				fprintf(outstream, " NIL");

			mime_findfield("content-disposition",
				       header_to_use, &mr);
			if (mr && strlen(mr->value) > 0) {
				fprintf(outstream, " (");
				show_mime_parameter_list(outstream, mr, 0,
							 0);
				fprintf(outstream, ")");
			} else
				fprintf(outstream, " NIL");

			mime_findfield("content-language", header_to_use,
				       &mr);
			if (mr && strlen(mr->value) > 0) {
				fprintf(outstream, " ");
				quoted_string_out(outstream, mr->value);
			} else
				fprintf(outstream, " NIL");
		}
	} else {
		/* check for a multipart message */
		if (is_rfc_multipart || is_mime_multipart) {
			curr = list_getstart(&msg->children);
			while (curr) {
				if (retrieve_structure
				    (outstream,
				     (mime_message_t *) curr->data,
				     show_extension_data) == -1)
					return -1;

				curr = curr->nextnode;
			}

			/* show multipart subtype */
			if (is_mime_multipart)
				mime_findfield("content-type",
					       &msg->mimeheader, &mr);
			else
				mime_findfield("content-type",
					       &msg->rfcheader, &mr);

			subtype = strchr(mr->value, '/');
			extension = strchr(subtype, ';');

			if (!subtype)
				fprintf(outstream, " NIL");
			else {
				if (!extension) {
					newline = strchr(subtype, '\n');
					if (!newline)
						return -1;

					*newline = 0;
					fprintf(outstream, " ");
					quoted_string_out(outstream,
							  subtype + 1);
					*newline = '\n';
				} else {
					*extension = 0;
					fprintf(outstream, " ");
					quoted_string_out(outstream,
							  subtype + 1);
					*extension = ';';
				}
			}

			/* show extension data (after subtype) */
			if (extension && show_extension_data) {
				show_mime_parameter_list(outstream, mr, 0,
							 1);

				/* FIXME: should give body-disposition & body-language here */
				fprintf(outstream, " NIL NIL");
			}
		} else {
			/* ??? */
		}
	}
	fprintf(outstream, ")");

	return 0;
}


/*
 * retrieve_envelope()
 *
 * retrieves the body envelope of an RFC-822 msg
 *
 * returns -1 on error, 0 on success
 */
int retrieve_envelope(FILE * outstream, struct list *rfcheader)
{
	struct mime_record *mr;
	int idx;

	fprintf(outstream, "(");

	mime_findfield("date", rfcheader, &mr);
	if (mr && strlen(mr->value) > 0) {
		quoted_string_out(outstream, mr->value);
		fprintf(outstream, " ");
	} else
		fprintf(outstream, "NIL ");

	mime_findfield("subject", rfcheader, &mr);
	if (mr && strlen(mr->value) > 0) {
		quoted_string_out(outstream, mr->value);
		fprintf(outstream, " ");
	} else
		fprintf(outstream, "NIL ");

	/* now from, sender, reply-to, to, cc, bcc, in-reply-to fields;
	 * note that multiple mailaddresses are separated by ','
	 */

	for (idx = 0; envelope_items[idx]; idx++) {
		mime_findfield(envelope_items[idx], rfcheader, &mr);
		if (mr && strlen(mr->value) > 0) {
			show_address_list(outstream, mr);
		} else if (strcasecmp(envelope_items[idx], "reply-to") ==
			   0) {
			/* default this field */
			mime_findfield("from", rfcheader, &mr);
			if (mr && strlen(mr->value) > 0)
				show_address_list(outstream, mr);
			else	/* no from field ??? */
				fprintf(outstream,
					"((NIL NIL \"nobody\" \"nowhere.nirgendwo\"))");
		} else if (strcasecmp(envelope_items[idx], "sender") == 0) {
			/* default this field */
			mime_findfield("from", rfcheader, &mr);
			if (mr && strlen(mr->value) > 0)
				show_address_list(outstream, mr);
			else	/* no from field ??? */
				fprintf(outstream,
					"((NIL NIL \"nobody\" \"nowhere.nirgendwo\"))");
		} else
			fprintf(outstream, "NIL");

		fprintf(outstream, " ");
	}

	mime_findfield("in-reply-to", rfcheader, &mr);
	if (mr && strlen(mr->value) > 0) {
		quoted_string_out(outstream, mr->value);
		fprintf(outstream, " ");
	} else
		fprintf(outstream, "NIL ");

	mime_findfield("message-id", rfcheader, &mr);
	if (mr && strlen(mr->value) > 0)
		quoted_string_out(outstream, mr->value);
	else
		fprintf(outstream, "NIL");

	fprintf(outstream, ")");

	return 0;
}


/*
 * show_address_list()
 *
 * gives an address list, output to outstream
 */
int show_address_list(FILE * outstream, struct mime_record *mr)
{
	int delimiter, i, inquote, start, has_split;
	char savechar;

	fprintf(outstream, "(");

	/* find ',' to split up multiple addresses */
	delimiter = 0;

	do {
		fprintf(outstream, "(");

		start = delimiter;

		for (inquote = 0;
		     mr->value[delimiter] && !(mr->value[delimiter] == ','
					       && !inquote); delimiter++)
			if (mr->value[delimiter] == '\"')
				inquote ^= 1;

		if (mr->value[delimiter])
			mr->value[delimiter] = 0;	/* replace ',' by NULL-termination */
		else
			delimiter = -1;	/* this will be the last one */

		/* the address currently being processed is now contained within
		 * &mr->value[start] 'till first '\0'
		 */

		/* possibilities for the mail address:
		 * (1) name <user@domain>
		 * (2) <user@domain>
		 * (3) user@domain
		 * scan for '<' to determine which case we should be dealing with;
		 */

		for (i = start, inquote = 0;
		     mr->value[i] && !(mr->value[i] == '<' && !inquote);
		     i++)
			if (mr->value[i] == '\"')
				inquote ^= 1;

		if (mr->value[i]) {
			if (i > start + 2) {
				/* name is contained in &mr->value[start] untill &mr->value[i-2] */
				/* name might contain quotes */
				savechar = mr->value[i - 1];
				mr->value[i - 1] = '\0';	/* terminate string */

				quoted_string_out(outstream,
						  &mr->value[start]);

				mr->value[i - 1] = savechar;

			} else
				fprintf(outstream, "NIL");

			start = i + 1;	/* skip to after '<' */
		} else
			fprintf(outstream, "NIL");

		fprintf(outstream, " NIL ");	/* source route ?? smtp at-domain-list ?? */

		/* now display user domainname; &mr->value[start] is starting point */
		fprintf(outstream, "\"");

		/*
		 * added a check for whitespace within the address (not good)
		 */
		for (i = start, has_split = 0;
		     mr->value[i] && mr->value[i] != '>'
		     && !isspace(mr->value[i]); i++) {
			if (mr->value[i] == '@') {
				fprintf(outstream, "\" \"");
				has_split = 1;
			} else {
				if (mr->value[i] == '"')
					fprintf(outstream, "\\");
				fprintf(outstream, "%c", mr->value[i]);
			}
		}

		if (!has_split)
			fprintf(outstream, "\" \"\"");	/* '@' did not occur */
		else
			fprintf(outstream, "\"");

		if (delimiter > 0) {
			mr->value[delimiter++] = ',';	/* restore & prepare for next iteration */
			while (isspace(mr->value[delimiter]))
				delimiter++;
		}

		fprintf(outstream, ")");

	} while (delimiter > 0);

	fprintf(outstream, ")");

	return 0;
}



/*
 * show_mime_parameter_list()
 *
 * shows mime name/value pairs, output to outstream
 * 
 * if force_subtype != 0 'NIL' will be outputted if no subtype is specified
 * if only_extension != 0 only extension data (after first ';') will be shown
 */
int show_mime_parameter_list(FILE * outstream, struct mime_record *mr,
			     int force_subtype, int only_extension)
{
	int idx, delimiter, start, end;
	char *fieldvalue = (char *)my_malloc(sizeof(char *)*255);
	char *tmpstring = (char *)my_malloc(sizeof(char *)*255);
	
	/* find first delimiter */
	for (delimiter = 0;
	     mr->value[delimiter] && mr->value[delimiter] != ';';
	     delimiter++);

	/* are there non-whitespace chars after the delimiter?                    */
	/* looking for the case where the mime type ends with a ";"               */
	/* if it is of type "text" it must have a default character set generated */
	end = strlen(mr->value);
	for (start = delimiter + 1;
	     (isspace(mr->value[start]) == 0 && start <= end); start++);
	end = start - delimiter - 1;
	start = 0;
	if (end && strstr(mr->value, "text"))
		start++;

	if (mr->value[delimiter])
		mr->value[delimiter] = 0;
	else
		delimiter = -1;

	if (!only_extension) {
		/* find main type in value */
		for (idx = 0; mr->value[idx] && mr->value[idx] != '/';
		     idx++);

		if (mr->value[idx] && (idx < delimiter || delimiter == -1)) {
			mr->value[idx] = 0;

			quoted_string_out(outstream, mr->value);
			fprintf(outstream, " ");
			quoted_string_out(outstream, &mr->value[idx + 1]);

			mr->value[idx] = '/';
		} else {
			quoted_string_out(outstream, mr->value);
			fprintf(outstream, " %s",
				force_subtype ? "NIL" : "");
		}
	}

	if (delimiter >= 0) {
		/* extra parameters specified */
		mr->value[delimiter] = ';';
		idx = delimiter;

		fprintf(outstream, " (");

		if (start)
			fprintf(outstream, "\"CHARSET\" \"US-ASCII\"");
		/* extra params: <name>=<val> [; <name>=<val> [; ...etc...]]
		 * note that both name and val may or may not be enclosed by 
		 * either single or double quotation marks
		 */

		do {
			/* skip whitespace */
			for (idx++; isspace(mr->value[idx]); idx++);

			if (!mr->value[idx])
				break;	/* ?? */

			/* check if quotation marks are specified */
			if (mr->value[idx] == '\"' || mr->value[idx] == '\'') {
				start = ++idx;
				while (mr->value[idx] && mr->value[idx] != mr->value[start - 1])
					idx++;

				if (!mr->value[idx] || mr->value[idx + 1] != '=')	/* ?? no end quote */
					break;

				end = idx;
				idx += 2;	/* skip to after '=' */
			} else {
				start = idx;
				while (mr->value[idx] && mr->value[idx] != '=')
					idx++;

				if (!mr->value[idx])	/* ?? no value specified */
					break;

				end = idx;
				idx++;	/* skip to after '=' */
			}
			
			fprintf(outstream, "\"%.*s\" ", (end - start), &mr->value[start]);

			/* now process the value; practically same procedure */

			if (mr->value[idx] == '\"' || mr->value[idx] == '\'') {
				start = ++idx;
				while (mr->value[idx] && mr->value[idx] != mr->value[start - 1])
					idx++;

				if (!mr->value[idx])	/* ?? no end quote */
					break;

				end = idx;
				idx++;
			} else {
				start = idx;

				while (mr->value[idx] && !isspace(mr->value[idx]) && mr->value[idx] != ';')
					idx++;

				end = idx;
			}

			
			/* Thunderbird doesn't like mime-values that are wrapped, so
			 * we unwrap mime-values 
			 *
			 * this code assumes mime values are no longer than 255 chars.
			 *
			 * PJS
			 */
			
			
			snprintf(tmpstring,255,"\"%.*s\"", (end - start), &mr->value[start]);
			mime_unwrap(fieldvalue,tmpstring);
			fprintf(outstream,fieldvalue);
			//fprintf(outstream, "\"%.*s\"", (end - start), &mr->value[start]);
			
			/* check for more name/val pairs */
			while (mr->value[idx] && mr->value[idx] != ';')
				idx++;

			if (mr->value[idx])
				fprintf(outstream, " ");

		} while (mr->value[idx]);

		fprintf(outstream, ")");

	} else {
		fprintf(outstream, " NIL");
	}
	my_free(fieldvalue);
	my_free(tmpstring);
			
	return 0;
}


/* 
 * get_part_by_num()
 *
 * retrieves a msg part by it's numeric specifier
 * 'part' is assumed to be valid! (i.e '1.2.3.44')
 * returns NULL if there is no such part 
 */
mime_message_t *get_part_by_num(mime_message_t * msg, const char *part)
{
	int nextpart, j;
	char *endptr;
	struct element *curr;

	if (part == NULL || strlen(part) == 0 || msg == NULL)
		return msg;
	trace(TRACE_DEBUG,"%s,%s: partspec [%s]", __FILE__, __func__, part);

	nextpart = strtoul(part, &endptr, 10);	/* strtoul() stops at '.' */

	for (j = 1, curr = list_getstart(&msg->children);
	     j < nextpart && curr; j++, curr = curr->nextnode);

	if (!curr)
		return NULL;

	if (*endptr)
		return get_part_by_num((mime_message_t *) curr->data, &endptr[1]);	/* skip dot in part */

	return (mime_message_t *) curr->data;
}


/*
 * rfcheader_dump()
 * 
 * dumps rfc-header fields belonging to rfcheader
 * the fields to be dumped are specified in fieldnames, an array containing nfields items
 *
 * if equal_type == 0 the field match criterium is inverted and non-matching fieldnames
 * will be selected
 *
 * to select every headerfield it suffices to set nfields and equal_type to 0
 *
 * returns number of bytes written to outmem
 */
u64_t rfcheader_dump(MEM * outmem, struct list * rfcheader,
		     char **fieldnames, int nfields, int equal_type)
{
	struct mime_record *mr;
	struct element *curr;
	u64_t size = 0;

	curr = list_getstart(rfcheader);
	if (rfcheader == NULL || curr == NULL) {
		/*size += fprintf(outstream, "NIL\r\n"); */
		return 0;
	}

	curr = list_getstart(rfcheader);
	while (curr) {
		mr = (struct mime_record *) curr->data;

		if (haystack_find(nfields, fieldnames, mr->field) ==
		    equal_type) {
			/* ok output this field */
			size +=
			    mwrite(mr->field, strlen(mr->field), outmem);
			size += mwrite(": ", 2, outmem);
			size +=
			    mwrite(mr->value, strlen(mr->value), outmem);
			size += mwrite("\r\n", 2, outmem);
		}

		curr = curr->nextnode;
	}
	size += mwrite("\r\n", 2, outmem);

	return size;
}


/*
 * mimeheader_dump()
 * 
 * dumps mime-header fields belonging to mimeheader
 *
 */
u64_t mimeheader_dump(MEM * outmem, struct list * mimeheader)
{
	struct mime_record *mr;
	struct element *curr;
	u64_t size = 0;

	curr = list_getstart(mimeheader);
	if (mimeheader == NULL || curr == NULL) {
		/*size = fprintf(outstream, "NIL\r\n"); */
		return 0;
	}

	while (curr) {
		mr = (struct mime_record *) curr->data;
		size += mwrite(mr->field, strlen(mr->field), outmem);
		size += mwrite(": ", 2, outmem);
		size += mwrite(mr->value, strlen(mr->value), outmem);
		size += mwrite("\r\n", 2, outmem);
		curr = curr->nextnode;
	}
	size += mwrite("\r\n", 2, outmem);

	return size;
}


/* 
 * find a string in an array of strings
 */
int haystack_find(int haystacklen, char **haystack, const char *needle)
{
	int i;

	for (i = 0; i < haystacklen; i++)
		if (strcasecmp(haystack[i], needle) == 0)
			return 1;

	return 0;
}




/*
 * build_args_array()
 *
 * builds an dimensional array of strings containing arguments based upon 
 * a series of arguments passed as a single string.
 * normal/square parentheses have special meaning:
 * '(body [all header])' will result in the following array:
 * [0] = '('
 * [1] = 'body'
 * [2] = '['
 * [3] = 'all'
 * [4] = 'header'
 * [5] = ']'
 * [6] = ')'
 *
 * quoted strings are those enclosed by double quotation marks and returned as a single argument
 * WITHOUT the enclosing quotation marks
 *
 * parentheses loose their special meaning if inside (double)quotation marks;
 * data should be 'clarified' (see clarify_data() function below)
 *
 * The returned array will be NULL-terminated.
 * Will return NULL upon errors.
 */

/* local defines */
#define NORMPAR 1
#define SQUAREPAR 2
#define NOPAR 0

char *the_args[MAX_ARGS];

char **build_args_array(const char *s)
{
	int nargs = 0, inquote = 0, i, quotestart = 0;
	int nnorm = 0, nsquare = 0, paridx = 0, slen = 0, argstart = 0;
	char parlist[MAX_LINESIZE];

	if (!s)
		return NULL;

	/* check for empty string */
	if (!(*s)) {
		the_args[0] = NULL;
		return the_args;
	}

	/* find the arguments */
	paridx = 0;
	parlist[paridx] = NOPAR;

	inquote = 0;
	slen = strlen(s);

	for (i = 0, nargs = 0; i < slen && nargs < MAX_ARGS - 1; i++) {
		/* check quotes */
		if (s[i] == '"' && ((i > 0 && s[i - 1] != '\\') || i == 0)) {
			if (inquote) {
				/* quotation end, treat quoted string as argument */
				if (!
				    (the_args[nargs] =
				     (char *) my_malloc(sizeof(char) *
							(i -
							 quotestart)))) {
					/* out of mem */
					while (--nargs >= 0) {
						my_free(the_args[nargs]);
						the_args[nargs] = NULL;
					}

					trace(TRACE_ERROR,
					      "IMAPD: Not enough memory while building up argument array.");
					return NULL;
				}

				memcpy((void *) the_args[nargs], 
				       (void *) &s[quotestart + 1],
				       i - quotestart - 1);
				the_args[nargs][i - quotestart - 1] = '\0';

				nargs++;
				inquote = 0;
			} else {
				inquote = 1;
				quotestart = i;
			}

			continue;
		}

		if (inquote)
			continue;

		/* check for (, ), [ or ] in string */
		if (s[i] == '(' || s[i] == ')' || s[i] == '['
		    || s[i] == ']') {
			/* check parenthese structure */
			if (s[i] == ')') {
				if (paridx < 0
				    || parlist[paridx] != NORMPAR)
					paridx = -1;
				else {
					nnorm--;
					paridx--;
				}
			} else if (s[i] == ']') {
				if (paridx < 0
				    || parlist[paridx] != SQUAREPAR)
					paridx = -1;
				else {
					paridx--;
					nsquare--;
				}
			} else if (s[i] == '(') {
				parlist[++paridx] = NORMPAR;
				nnorm++;
			} else {	/* s[i] == '[' */

				parlist[++paridx] = SQUAREPAR;
				nsquare++;
			}

			if (paridx < 0) {
				/* error in parenthesis structure */
				while (--nargs >= 0) {
					my_free(the_args[nargs]);
					the_args[nargs] = NULL;
				}
				return NULL;
			}

			/* add this parenthesis to the arg list and continue */
			if (!
			    (the_args[nargs] =
			     (char *) my_malloc(sizeof(" ")))) {
				/* out of mem */
				while (--nargs >= 0) {
					my_free(the_args[nargs]);
					the_args[nargs] = NULL;
				}

				trace(TRACE_ERROR,
				      "IMAPD: Not enough memory while building up argument array.");
				return NULL;
			}
			the_args[nargs][0] = s[i];
			the_args[nargs][1] = '\0';

			nargs++;
			continue;
		}

		if (s[i] == ' ')
			continue;

		/* at an argument start now, walk on until next delimiter
		 * and save argument 
		 */

		for (argstart = i; i < slen && !strchr(" []()", s[i]); i++)
			if (s[i] == '"') {
				if (s[i - 1] == '\\')
					continue;
				else
					break;
			}

		if (!
		    (the_args[nargs] =
		     (char *) my_malloc(sizeof(char) *
					(i - argstart + 1)))) {
			/* out of mem */
			while (--nargs >= 0) {
				my_free(the_args[nargs]);
				the_args[nargs] = NULL;
			}

			trace(TRACE_ERROR,
			      "IMAPD: Not enough memory while building up argument array.");
			return NULL;
		}

		memcpy((void *) the_args[nargs], (void *) &s[argstart], 
		       i - argstart);
		the_args[nargs][i - argstart] = '\0';

		nargs++;
		i--;		/* walked one too far */
	}

	if (paridx != 0) {
		/* error in parenthesis structure */
		while (--nargs >= 0) {
			my_free(the_args[nargs]);
			the_args[nargs] = NULL;
		}
		return NULL;
	}

	the_args[nargs] = NULL;	/* terminate */

	/* dump args (debug) */
	for (i = 0; the_args[i]; i++) {
		trace(TRACE_DEBUG, "arg[%d]: '%s'\n", i, the_args[i]);
	}

	return the_args;
}

/*
 * as build_args_array(), but reads strings on cmd line specified by {##}\0
 * (\r\n had been removed from string)
 */
char **build_args_array_ext(const char *originalString, clientinfo_t * ci)
{
	int nargs = 0, inquote = 0, quotestart = 0;
	int nnorm = 0, nsquare = 0, paridx = 0, argstart = 0;
	unsigned int i;
	char parlist[MAX_LINESIZE];
	char s[MAX_LINESIZE];
	char *tmp, *lastchar;
	int quotedSize, cnt, dataidx;

	/* this is done for the possible extra lines to be read from the client:
	 * the line is read into currline; s will always point to the line currently
	 * being processed
	 */
	strncpy(s, originalString, MAX_LINESIZE);

	if (!s)
		return NULL;

	/* check for empty string */
	if (!(*s)) {
		the_args[0] = NULL;
		return the_args;
	}

	/* find the arguments */
	paridx = 0;
	parlist[paridx] = NOPAR;

	inquote = 0;

	for (i = 0, nargs = 0; s[i] && nargs < MAX_ARGS - 1; i++) {
		/* check quotes */
		if (s[i] == '"' && ((i > 0 && s[i - 1] != '\\') || i == 0)) {
			if (inquote) {
				/* quotation end, treat quoted string as argument */
				if (!
				    (the_args[nargs] =
				     (char *) my_malloc(sizeof(char) *
							(i -
							 quotestart)))) {
					/* out of mem */
					while (--nargs >= 0) {
						my_free(the_args[nargs]);
						the_args[nargs] = NULL;
					}

					trace(TRACE_ERROR,
					      "IMAPD: Not enough memory while building up argument array.");
					return NULL;
				}

				memcpy((void *) the_args[nargs], 
				       (void *) &s[quotestart + 1],
				       i - quotestart - 1);
				the_args[nargs][i - quotestart - 1] = '\0';

				nargs++;
				inquote = 0;
			} else {
				inquote = 1;
				quotestart = i;
			}

			continue;
		}

		if (inquote)
			continue;

		/* check for (, ), [ or ] in string */
		if (s[i] == '(' || s[i] == ')' || s[i] == '['
		    || s[i] == ']') {
			/* check parenthese structure */
			if (s[i] == ')') {
				if (paridx < 0
				    || parlist[paridx] != NORMPAR)
					paridx = -1;
				else {
					nnorm--;
					paridx--;
				}
			} else if (s[i] == ']') {
				if (paridx < 0
				    || parlist[paridx] != SQUAREPAR)
					paridx = -1;
				else {
					paridx--;
					nsquare--;
				}
			} else if (s[i] == '(') {
				parlist[++paridx] = NORMPAR;
				nnorm++;
			} else {	/* s[i] == '[' */

				parlist[++paridx] = SQUAREPAR;
				nsquare++;
			}

			if (paridx < 0) {
				/* error in parenthesis structure */
				while (--nargs >= 0) {
					my_free(the_args[nargs]);
					the_args[nargs] = NULL;
				}
				return NULL;
			}

			/* add this parenthesis to the arg list and continue */
			if (!
			    (the_args[nargs] =
			     (char *) my_malloc(sizeof(" ")))) {
				/* out of mem */
				while (--nargs >= 0) {
					my_free(the_args[nargs]);
					the_args[nargs] = NULL;
				}

				trace(TRACE_ERROR,
				      "IMAPD: Not enough memory while building up argument array.");
				return NULL;
			}
			the_args[nargs][0] = s[i];
			the_args[nargs][1] = '\0';

			nargs++;
			continue;
		}

		if (s[i] == ' ')
			continue;

		/* check for {number}\0 */
		if (s[i] == '{') {
			quotedSize = strtoul(&s[i + 1], &lastchar, 10);

			/* only continue if the number is followed by '}\0' */
			trace(TRACE_DEBUG, "%s,%s: last char = %c", __FILE__, __func__, *lastchar);
			if ((*lastchar == '+' && *(lastchar + 1) == '}' && 
			     *(lastchar + 2) == '\0') || 
			    (*lastchar == '}' && *(lastchar + 1) == '\0')) {
				/* allocate space for this argument (could be a message when used with APPEND) */
				if (!
				    (the_args[nargs] =
				     (char *) my_malloc(sizeof(char) *
							(quotedSize +
							 1)))) {
					/* out of mem */
					while (--nargs >= 0) {
						my_free(the_args[nargs]);
						the_args[nargs] = NULL;
					}

					trace(TRACE_ERROR,
					      "build_args_array_ext(): out of memory allocating [%u] bytes for extra string",
					      quotedSize + 1);
					return NULL;
				}

				fprintf(ci->tx,
					"+ OK gimme that string\r\n");
				alarm(ci->timeout);	/* dont wait forever */
				for (cnt = 0, dataidx = 0;
				     cnt < quotedSize; cnt++) {
					the_args[nargs][dataidx] =
					    fgetc(ci->rx);

					if (the_args[nargs][dataidx] !=
					    '\r')
						dataidx++;	/* only store if it is not \r */
				}

				alarm(0);
				the_args[nargs][dataidx] = '\0';	/* terminate string */
				nargs++;

				if (!ci->rx || !ci->tx || ferror(ci->rx)
				    || ferror(ci->tx)) {
					/* timeout occurred or connection has gone away */
					while (--nargs >= 0) {
						my_free(the_args[nargs]);
						the_args[nargs] = NULL;
					}

					trace(TRACE_ERROR,
					      "build_args_array_ext(): timeout occurred");
					return NULL;
				}

				/* now read the rest of this line */
				alarm(ci->timeout);
				fgets(s, MAX_LINESIZE, ci->rx);
				alarm(0);

				if (!ci->rx || !ci->tx || ferror(ci->rx)
				    || ferror(ci->tx)) {
					/* timeout occurred */
					while (--nargs >= 0) {
						my_free(the_args[nargs]);
						the_args[nargs] = NULL;
					}

					trace(TRACE_ERROR,
					      "build_args_array_ext(): timeout occurred");
					return NULL;
				}

				/* remove trailing \r\n */
				tmp = &s[strlen(s)];
				tmp--;	/* go before trailing \0; watch this with empty strings! */
				while (tmp >= s
				       && (*tmp == '\r' || *tmp == '\n')) {
					*tmp = '\0';
					tmp--;
				}

				trace(TRACE_DEBUG,
				      "build_args_array_ext(): got extra line [%s]",
				      s);

				/* start over! */
				i = 0;
				continue;
			}
		}

		/* at an argument start now, walk on until next delimiter
		 * and save argument 
		 */

		for (argstart = i; i < strlen(s) && !strchr(" []()", s[i]);
		     i++)
			if (s[i] == '"') {
				if (s[i - 1] == '\\')
					continue;
				else
					break;
			}

		if (!
		    (the_args[nargs] =
		     (char *) my_malloc(sizeof(char) *
					(i - argstart + 1)))) {
			/* out of mem */
			while (--nargs >= 0) {
				my_free(the_args[nargs]);
				the_args[nargs] = NULL;
			}

			trace(TRACE_ERROR,
			      "IMAPD: Not enough memory while building up argument array.");
			return NULL;
		}

		memcpy((void *) the_args[nargs], (void *) &s[argstart], 
		       i - argstart);
		the_args[nargs][i - argstart] = '\0';

		nargs++;
		i--;		/* walked one too far */
	}

	if (paridx != 0) {
		/* error in parenthesis structure */
		while (--nargs >= 0) {
			my_free(the_args[nargs]);
			the_args[nargs] = NULL;
		}
		return NULL;
	}

	the_args[nargs] = NULL;	/* terminate */

	/* dump args (debug) */
	for (i = 0; the_args[i]; i++) {
		trace(TRACE_DEBUG, "arg[%d]: '%s'\n", i, the_args[i]);
	}

	return the_args;
}

#undef NOPAR
#undef NORMPAR
#undef RIGHTPAR


/*
 * clarify_data()
 *
 * replaces all multiple spaces by a single one except for quoted spaces;
 * removes leading and trailing spaces and a single trailing newline (if present)
 */
void clarify_data(char *str)
{
	int startidx, inquote, endidx;
	unsigned int i;


	/* remove leading spaces */
	for (i = 0; str[i] == ' '; i++);
	memmove(str, &str[i], sizeof(char) * (strlen(&str[i]) + 1));	/* add one for \0 */

	/* remove CR/LF */
	endidx = strlen(str) - 1;
	if (endidx >= 0 && (str[endidx] == '\n' || str[endidx] == '\r'))
		endidx--;

	if (endidx >= 0 && (str[endidx] == '\n' || str[endidx] == '\r'))
		endidx--;


	if (endidx == 0) {
		/* only 1 char left and it is not a space */
		str[1] = '\0';
		return;
	}

	/* remove trailing spaces */
	for (i = endidx; i > 0 && str[i] == ' '; i--);
	if (i == 0) {
		/* empty string remains */
		*str = '\0';
		return;
	}

	str[i + 1] = '\0';

	/* scan for multiple spaces */
	inquote = 0;
	for (i = 0; i < strlen(str); i++) {
		if (str[i] == '"') {
			if ((i > 0 && str[i - 1] != '\\') || i == 0) {
				/* toggle in-quote flag */
				inquote ^= 1;
			}
		}

		if (str[i] == ' ' && !inquote) {
			for (startidx = i; str[i] == ' '; i++);

			if (i - startidx > 1) {
				/* multiple non-quoted spaces found --> remove 'm */
				memmove(&str[startidx + 1], &str[i],
					sizeof(char) * (strlen(&str[i]) +
							1));
				/* update i */
				i = startidx + 1;
			}
		}
	}
}


/*
 * is_textplain()
 *
 * checks if content-type is text/plain
 */
int is_textplain(struct list *hdr)
{
	struct mime_record *mr;
	int i, len;

	if (!hdr)
		return 0;

	mime_findfield("content-type", hdr, &mr);

	if (!mr)
		return 0;

	len = strlen(mr->value);
	for (i = 0; len - i >= (int) sizeof("text/plain"); i++)
		if (strncasecmp
		    (&mr->value[i], "text/plain",
		     sizeof("text/plain") - 1) == 0)
			return 1;

	return 0;
}


/*
 * convert a mySQL date (yyyy-mm-dd hh:mm:ss) to a valid IMAP internal date:
 *                       0123456789012345678
 * dd-mon-yyyy hh:mm:ss with mon characters (i.e. 'Apr' for april)
 * 01234567890123456789
 * return value is valid until next function call.
 * NOTE: if date is not valid, IMAP_STANDARD_DATE is returned
 */
char *date_sql2imap(const char *sqldate)
{
	char *last_char;
	struct tm tm_localtime, tm_sqldate;
	time_t td;

	/* we need to get the localtime to get the current timezone */
	if (time(&td) == -1) {
		trace(TRACE_ERROR, "%s,%s: error getting time()",
		      __FILE__, __func__);
		return IMAP_STANDARD_DATE;
	}
	tm_localtime = *localtime(&td);

	/* parse sqldate */
	last_char = strptime(sqldate, "%Y-%m-%d %T", &tm_sqldate);
	if (last_char == NULL || *last_char != '\0') {
		trace(TRACE_DEBUG, "%s,%s, error parsing date [%s]",
		      __FILE__, __func__, sqldate);
		strcpy(_imapdate, IMAP_STANDARD_DATE);
		return _imapdate;
	}
	/* copy timezone + daylight savings time info from localtime */
	tm_sqldate.tm_gmtoff = tm_localtime.tm_gmtoff;
	tm_sqldate.tm_isdst = tm_localtime.tm_isdst;

	(void) strftime(_imapdate, IMAP_INTERNALDATE_LEN, 
			"%d-%b-%Y %T %z", &tm_sqldate);

	return _imapdate;
}


/*
 * convert TO a mySQL date (yyyy-mm-dd) FROM a valid IMAP internal date:
 *                          0123456789
 * dd-mon-yyyy with mon characters (i.e. 'Apr' for april)
 * 01234567890
 * OR
 * d-mon-yyyy
 * return value is valid until next function call.
 */
char *date_imap2sql(const char *imapdate)
{
	struct tm tm;
	char *last_char;

	last_char = strptime(imapdate, "%d-%b-%Y", &tm);
	if (last_char == NULL || *last_char != '\0') {
		trace(TRACE_DEBUG, "%s,%s: error parsing IMAP date %s",
		      __FILE__, __func__, imapdate);
		return NULL;
	}
	(void) strftime(_sqldate, SQL_INTERNALDATE_LEN,
			"%Y-%m-%d 00:00:00", &tm);

	return _sqldate;
}

/*
 *
 */
size_t stridx(const char *s, char ch)
{
	size_t i;

	for (i = 0; s[i] && s[i] != ch; i++);

	return i;
}


/*
 * checkchars()
 *
 * performs a check to see if the read data is valid
 * returns 0 if invalid, 1 otherwise
 */
int checkchars(const char *s)
{
	int i;

	for (i = 0; s[i]; i++) {
		if (!strchr(AcceptedChars, s[i])) {
			/* wrong char found */
			return 0;
		}
	}
	return 1;
}


/*
 * checktag()
 *
 * performs a check to see if the read data is valid
 * returns 0 if invalid, 1 otherwise
 */
int checktag(const char *s)
{
	int i;

	for (i = 0; s[i]; i++) {
		if (!strchr(AcceptedTagChars, s[i])) {
			/* wrong char found */
			return 0;
		}
	}
	return 1;
}


/*
 * checkmailboxname()
 *
 * performs a check to see if the mailboxname is valid
 * returns 0 if invalid, 1 otherwise
 */
int checkmailboxname(const char *s)
{
	int i;

	if (strlen(s) == 0)
		return 0;	/* empty name is not valid */

	if (strlen(s) >= IMAP_MAX_MAILBOX_NAMELEN)
		return 0;	/* a too large string is not valid */

	/* check for invalid characters */
	for (i = 0; s[i]; i++) {
		if (!strchr(AcceptedMailboxnameChars, s[i])) {
			/* dirty hack to allow namespaces to function */
			if (i == 0 && s[0] == '#')
				continue;
			/* wrong char found */
			return 0;
		}
	}

	/* check for double '/' */
	for (i = 1; s[i]; i++) {
		if (s[i] == '/' && s[i - 1] == '/')
			return 0;
	}

	/* check if the name consists of a single '/' */
	if (strlen(s) == 1 && s[0] == '/')
		return 0;

	return 1;
}


/*
 * check_date()
 *
 * checks a date for IMAP-date validity:
 * dd-MMM-yyyy
 * 01234567890
 * month three-letter specifier
 */
int check_date(const char *date)
{
	char sub[4];
	int days, i, j;

	if (strlen(date) != strlen("01-Jan-1970")
	    && strlen(date) != strlen("1-Jan-1970"))
		return 0;

	j = (strlen(date) == strlen("1-Jan-1970")) ? 1 : 0;

	if (date[2 - j] != '-' || date[6 - j] != '-')
		return 0;

	days = strtoul(date, NULL, 10);
	strncpy(sub, &date[3 - j], 3);
	sub[3] = 0;

	for (i = 0; i < 12; i++) {
		if (strcasecmp(month_desc[i], sub) == 0)
			break;
	}

	if (i >= 12 || days > month_len[i])
		return 0;

	for (i = 7; i < 11; i++)
		if (!isdigit(date[i - j]))
			return 0;

	return 1;
}



/*
 * check_msg_set()
 *
 * checks if s represents a valid message set 
 */
int check_msg_set(const char *s)
{
	int i, indigit;

	if (!s || !isdigit(s[0]))
		return 0;

	for (i = 1, indigit = 1; s[i]; i++) {
		if (isdigit(s[i]))
			indigit = 1;
		else if (s[i] == ',') {
			if (!indigit && s[i - 1] != '*')
				return 0;

			indigit = 0;
		} else if (s[i] == ':') {
			if (!indigit)
				return 0;

			indigit = 0;
		} else if (s[i] == '*') {
			if (s[i - 1] != ':')
				return 0;
		}
	}

	return 1;
}


/*
 * base64encode()
 *
 * encodes a string using base64 encoding
 */
void base64encode(char *in, char *out)
{
	for (; strlen(in) >= 3; in += 3) {
		*out++ = base64encodestring[(in[0] & 0xFC) >> 2U];
		*out++ =
		    base64encodestring[((in[0] & 0x03) << 4U) |
				       ((in[1] & 0xF0) >> 4U)];
		*out++ =
		    base64encodestring[((in[1] & 0x0F) << 2U) |
				       ((in[2] & 0xC0) >> 6U)];
		*out++ = base64encodestring[(in[2] & 0x3F)];
	}

	if (strlen(in) == 2) {
		/* 16 bits left to encode */
		*out++ = base64encodestring[(in[0] & 0xFC) >> 2U];
		*out++ =
		    base64encodestring[((in[0] & 0x03) << 4U) |
				       ((in[1] & 0xF0) >> 4U)];
		*out++ = base64encodestring[((in[1] & 0x0F) << 2U)];
		*out++ = '=';

		return;
	}

	if (strlen(in) == 1) {
		/* 8 bits left to encode */
		*out++ = base64encodestring[(in[0] & 0xFC) >> 2U];
		*out++ = base64encodestring[((in[0] & 0x03) << 4U)];
		*out++ = '=';
		*out++ = '=';

		return;
	}
}


/*
 * base64decode()
 *
 * decodes a base64 encoded string
 */
void base64decode(char *in, char *out)
{
	for (; strlen(in) >= 4; in += 4) {
		*out++ = (stridx(base64encodestring, in[0]) << 2U)
		    | ((stridx(base64encodestring, in[1]) & 0x30) >> 4U);

		*out++ = ((stridx(base64encodestring, in[1]) & 0x0F) << 4U)
		    | ((stridx(base64encodestring, in[2]) & 0x3C) >> 2U);

		*out++ = ((stridx(base64encodestring, in[2]) & 0x03) << 6U)
		    | (stridx(base64encodestring, in[3]) & 0x3F);
	}

	*out = 0;
}


/*
 * binary_search()
 *
 * performs a binary search on array to find key
 * array should be ascending in values
 *
 * returns -1 if not found. key_idx will hold key if found
 */
int binary_search(const u64_t * array, unsigned arraysize, u64_t key,
		  unsigned int *key_idx)
{
	unsigned low, high, mid = 1;

	assert(key_idx != NULL);
	*key_idx = 0;
	if (arraysize == 0)
		return -1;

	low = 0;
	high = arraysize - 1;

	while (low <= high) {
		mid = (high + low) / (unsigned) 2;
		if (array[mid] < key)
			low = mid + 1;
		else if (array[mid] > key) {
			if (mid > 0)
				high = mid - 1;
			else
				break;
		} else {
			*key_idx = mid;
			return 1;
		}
	}

	return -1;		/* not found */
}

/* 
 * sends a string to outstream, escaping the following characters:
 * "  --> \"
 * \  --> \\
 *
 * double quotes are placed at the beginning and end of the string.
 *
 * returns the number of bytes outputted.
 */
int quoted_string_out(FILE * outstream, const char *s)
{
	int i, cnt;

	// check wheter we must use literal string
	for (i = 0; s[i]; i++) {
		if (!(s[i] & 0xe0) || (s[i] & 0x80) || (s[i] == '"')
		    || (s[i] == '\\')) {
			cnt = fprintf(outstream, "{");
			cnt +=
			    fprintf(outstream, "%lu",
				    (unsigned long) strlen(s));
			cnt += fprintf(outstream, "}\r\n");
			cnt += fprintf(outstream, "%s", s);
			return cnt;
		}
	}

	cnt = fprintf(outstream, "\"");
	cnt += fprintf(outstream, "%s", s);
	cnt += fprintf(outstream, "\"");

	return cnt;
}


/*
 * send_data()
 *
 * sends cnt bytes from a MEM structure to a FILE stream
 * uses a simple buffering system
 */
void send_data(FILE * to, MEM * from, int cnt)
{
	char buf[SEND_BUF_SIZE];

	for (cnt -= SEND_BUF_SIZE; cnt >= 0; cnt -= SEND_BUF_SIZE) {
		mread(buf, SEND_BUF_SIZE, from);
		fwrite(buf, SEND_BUF_SIZE, 1, to);
	}

	if (cnt < 0) {
		mread(buf, cnt + SEND_BUF_SIZE, from);
		fwrite(buf, cnt + SEND_BUF_SIZE, 1, to);
	}

	fflush(to);
}



/*
 * build_imap_search()
 *
 * builds a linked list of search items from a set of IMAP search keys
 * sl should be initialized; new search items are simply added to the list
 *
 * returns -1 on syntax error, -2 on memory error; 0 on success, 1 if ')' has been encountered
 */
int build_imap_search(char **search_keys, struct list *sl, int *idx, int sorted)
{
	search_key_t key;
	int result;

	if (!search_keys || !search_keys[*idx])
		return 0;

	memset(&key, 0, sizeof(key));
	/* coming from _ic_sort */
	if(sorted && (strcasecmp(search_keys[*idx], "arrival") == 0)) {
		key.type = IST_SORT;
		strncpy(key.search, "order by pms.internal_date", MAX_SEARCH_LEN);
		(*idx)++;
	} else if(sorted && (strcasecmp(search_keys[*idx], "from") == 0)) {
		key.type = IST_SORTHDR;
		strncpy(key.hdrfld, "from", MIME_FIELD_MAX);
		(*idx)++;
	} else if(sorted && (strcasecmp(search_keys[*idx], "subject") == 0)) {
		key.type = IST_SORTHDR;
		strncpy(key.hdrfld, "subject", MIME_FIELD_MAX);
		(*idx)++;
	} else if(sorted && (strcasecmp(search_keys[*idx], "cc") == 0)) {
		key.type = IST_SORTHDR;
		strncpy(key.hdrfld, "cc", MIME_FIELD_MAX);
		(*idx)++;
	} else if(sorted && (strcasecmp(search_keys[*idx], "to") == 0)) {
		key.type = IST_SORTHDR;
		strncpy(key.hdrfld, "to", MIME_FIELD_MAX);
		(*idx)++;
	} else if(sorted && (strcasecmp(search_keys[*idx], "reverse") == 0)) {
		/* TODO */ 
		(*idx)++;
	} else if(sorted && (strcasecmp(search_keys[*idx], "size") == 0)) {
		/* TODO */ 
		(*idx)++;
	} else if(sorted && (strcasecmp(search_keys[*idx], "iso-8859-1") == 0)) {
		/* TODO */ 
		(*idx)++;
	} else if(sorted && (strcasecmp(search_keys[*idx], "date") == 0)) {
		/* TODO */ 
		(*idx)++;
	} else if(sorted && (strcasecmp(search_keys[*idx], "all") == 0)) {
		/* TODO */ 
		(*idx)++;
	} else if (strcasecmp(search_keys[*idx], "all") == 0) {
		key.type = IST_SET;
		strcpy(key.search, "1:*");
		(*idx)++;
	} else if (strcasecmp(search_keys[*idx], "uid") == 0) {
		key.type = IST_SET_UID;
		if (!search_keys[*idx + 1])
			return -1;

		(*idx)++;

		strncpy(key.search, search_keys[(*idx)], MAX_SEARCH_LEN);
		if (!check_msg_set(key.search))
			return -1;

		(*idx)++;
	}

	/*
	 * FLAG search keys
	 */

	else if (strcasecmp(search_keys[*idx], "answered") == 0) {
		key.type = IST_FLAG;
		strncpy(key.search, "answered_flag=1", MAX_SEARCH_LEN);
		(*idx)++;
	} else if (strcasecmp(search_keys[*idx], "deleted") == 0) {
		key.type = IST_FLAG;
		strncpy(key.search, "deleted_flag=1", MAX_SEARCH_LEN);
		(*idx)++;
	} else if (strcasecmp(search_keys[*idx], "flagged") == 0) {
		key.type = IST_FLAG;
		strncpy(key.search, "flagged_flag=1", MAX_SEARCH_LEN);
		(*idx)++;
	} else if (strcasecmp(search_keys[*idx], "recent") == 0) {
		key.type = IST_FLAG;
		strncpy(key.search, "recent_flag=1", MAX_SEARCH_LEN);
		(*idx)++;
	} else if (strcasecmp(search_keys[*idx], "seen") == 0) {
		key.type = IST_FLAG;
		strncpy(key.search, "seen_flag=1", MAX_SEARCH_LEN);
		(*idx)++;
	} else if (strcasecmp(search_keys[*idx], "keyword") == 0) {
		/* no results from this one */
		if (!search_keys[(*idx) + 1])	/* there should follow an argument */
			return -1;

		(*idx)++;

		key.type = IST_SET;
		strcpy(key.search, "0");
		(*idx)++;
	} else if (strcasecmp(search_keys[*idx], "draft") == 0) {
		key.type = IST_FLAG;
		strncpy(key.search, "draft_flag=1", MAX_SEARCH_LEN);
		(*idx)++;
	} else if (strcasecmp(search_keys[*idx], "new") == 0) {
		key.type = IST_FLAG;
		strncpy(key.search, "(seen_flag=0 AND recent_flag=1)",
			MAX_SEARCH_LEN);
		(*idx)++;
	} else if (strcasecmp(search_keys[*idx], "old") == 0) {
		key.type = IST_FLAG;
		strncpy(key.search, "recent_flag=0", MAX_SEARCH_LEN);
		(*idx)++;
	} else if (strcasecmp(search_keys[*idx], "unanswered") == 0) {
		key.type = IST_FLAG;
		strncpy(key.search, "answered_flag=0", MAX_SEARCH_LEN);
		(*idx)++;
	} else if (strcasecmp(search_keys[*idx], "undeleted") == 0) {
		key.type = IST_FLAG;
		strncpy(key.search, "deleted_flag=0", MAX_SEARCH_LEN);
		(*idx)++;
	} else if (strcasecmp(search_keys[*idx], "unflagged") == 0) {
		key.type = IST_FLAG;
		strncpy(key.search, "flagged_flag=0", MAX_SEARCH_LEN);
		(*idx)++;
	} else if (strcasecmp(search_keys[*idx], "unseen") == 0) {
		key.type = IST_FLAG;
		strncpy(key.search, "seen_flag=0", MAX_SEARCH_LEN);
		(*idx)++;
	} else if (strcasecmp(search_keys[*idx], "unkeyword") == 0) {
		/* matches every msg */
		if (!search_keys[(*idx) + 1])
			return -1;

		(*idx)++;

		key.type = IST_SET;
		strcpy(key.search, "1:*");
		(*idx)++;
	} else if (strcasecmp(search_keys[*idx], "undraft") == 0) {
		key.type = IST_FLAG;
		strncpy(key.search, "draft_flag=0", MAX_SEARCH_LEN);
		(*idx)++;
	}

	/*
	 * HEADER search keys
	 */

	else if (strcasecmp(search_keys[*idx], "bcc") == 0) {
		key.type = IST_HDR;
		strncpy(key.hdrfld, "bcc", MIME_FIELD_MAX);
		if (!search_keys[(*idx) + 1])
			return -1;

		(*idx)++;
		strncpy(key.search, search_keys[(*idx)], MAX_SEARCH_LEN);
		(*idx)++;
	} else if (strcasecmp(search_keys[*idx], "cc") == 0) {
		key.type = IST_HDR;
		strncpy(key.hdrfld, "cc", MIME_FIELD_MAX);
		if (!search_keys[(*idx) + 1])
			return -1;

		(*idx)++;
		strncpy(key.search, search_keys[(*idx)], MAX_SEARCH_LEN);
		(*idx)++;
	} else if (strcasecmp(search_keys[*idx], "from") == 0) {
		key.type = IST_HDR;
		strncpy(key.hdrfld, "from", MIME_FIELD_MAX);
		if (!search_keys[(*idx) + 1])
			return -1;

		(*idx)++;
		strncpy(key.search, search_keys[(*idx)], MAX_SEARCH_LEN);
		(*idx)++;
	} else if (strcasecmp(search_keys[*idx], "to") == 0) {
		key.type = IST_HDR;
		strncpy(key.hdrfld, "to", MIME_FIELD_MAX);
		if (!search_keys[(*idx) + 1])
			return -1;

		(*idx)++;
		strncpy(key.search, search_keys[(*idx)], MAX_SEARCH_LEN);
		(*idx)++;
	} else if (strcasecmp(search_keys[*idx], "subject") == 0) {
		key.type = IST_HDR;
		strncpy(key.hdrfld, "subject", MIME_FIELD_MAX);
		if (!search_keys[(*idx) + 1])
			return -1;

		(*idx)++;
		strncpy(key.search, search_keys[(*idx)], MAX_SEARCH_LEN);
		(*idx)++;
	} else if (strcasecmp(search_keys[*idx], "header") == 0) {
		key.type = IST_HDR;
		if (!search_keys[(*idx) + 1] || !search_keys[(*idx) + 2])
			return -1;

		strncpy(key.hdrfld, search_keys[(*idx) + 1],
			MIME_FIELD_MAX);
		strncpy(key.search, search_keys[(*idx) + 2],
			MAX_SEARCH_LEN);

		(*idx) += 3;
	} else if (strcasecmp(search_keys[*idx], "sentbefore") == 0) {
		key.type = IST_HDRDATE_BEFORE;
		strncpy(key.hdrfld, "date", MIME_FIELD_MAX);
		if (!search_keys[(*idx) + 1])
			return -1;

		(*idx)++;
		strncpy(key.search, search_keys[(*idx)], MAX_SEARCH_LEN);
		(*idx)++;
	} else if (strcasecmp(search_keys[*idx], "senton") == 0) {
		key.type = IST_HDRDATE_ON;
		strncpy(key.hdrfld, "date", MIME_FIELD_MAX);
		if (!search_keys[(*idx) + 1])
			return -1;

		(*idx)++;
		strncpy(key.search, search_keys[(*idx)], MAX_SEARCH_LEN);
		(*idx)++;
	} else if (strcasecmp(search_keys[*idx], "sentsince") == 0) {
		key.type = IST_HDRDATE_SINCE;
		strncpy(key.hdrfld, "date", MIME_FIELD_MAX);
		if (!search_keys[(*idx) + 1])
			return -1;

		(*idx)++;
		strncpy(key.search, search_keys[(*idx)], MAX_SEARCH_LEN);
		(*idx)++;
	}

	/*
	 * INTERNALDATE keys
	 */

	else if (strcasecmp(search_keys[*idx], "before") == 0) {
		key.type = IST_IDATE;
		if (!search_keys[(*idx) + 1])
			return -1;

		(*idx)++;
		if (!check_date(search_keys[*idx]))
			return -1;

		strncpy(key.search, "internal_date<'", MAX_SEARCH_LEN);
		strncat(key.search, date_imap2sql(search_keys[*idx]),
			MAX_SEARCH_LEN - sizeof("internal_date<''"));
		strcat(key.search, "'");

		(*idx)++;
	} else if (strcasecmp(search_keys[*idx], "on") == 0) {
		key.type = IST_IDATE;
		if (!search_keys[(*idx) + 1])
			return -1;

		(*idx)++;
		if (!check_date(search_keys[*idx]))
			return -1;

		strncpy(key.search, "internal_date LIKE '",
			MAX_SEARCH_LEN);
		strncat(key.search, date_imap2sql(search_keys[*idx]),
			MAX_SEARCH_LEN - sizeof("internal_date LIKE 'x'"));
		strcat(key.search, "%'");

		(*idx)++;
	} else if (strcasecmp(search_keys[*idx], "since") == 0) {
		key.type = IST_IDATE;
		if (!search_keys[(*idx) + 1])
			return -1;

		(*idx)++;
		if (!check_date(search_keys[*idx]))
			return -1;

		strncpy(key.search, "internal_date>'", MAX_SEARCH_LEN);
		strncat(key.search, date_imap2sql(search_keys[*idx]),
			MAX_SEARCH_LEN - sizeof("internal_date>''"));
		strcat(key.search, "'");

		(*idx)++;
	}

	/*
	 * DATA-keys
	 */

	else if (strcasecmp(search_keys[*idx], "body") == 0) {
		key.type = IST_DATA_BODY;
		if (!search_keys[(*idx) + 1])
			return -1;

		(*idx)++;
		strncpy(key.search, search_keys[(*idx)], MAX_SEARCH_LEN);
		(*idx)++;
	} else if (strcasecmp(search_keys[*idx], "text") == 0) {
		key.type = IST_DATA_TEXT;
		if (!search_keys[(*idx) + 1])
			return -1;

		(*idx)++;
		strncpy(key.search, search_keys[(*idx)], MAX_SEARCH_LEN);
		(*idx)++;
	}

	/*
	 * SIZE keys
	 */

	else if (strcasecmp(search_keys[*idx], "larger") == 0) {
		key.type = IST_SIZE_LARGER;
		if (!search_keys[(*idx) + 1])
			return -1;

		(*idx)++;
		key.size = strtoull(search_keys[(*idx)], NULL, 10);
		(*idx)++;
	} else if (strcasecmp(search_keys[*idx], "smaller") == 0) {
		key.type = IST_SIZE_SMALLER;
		if (!search_keys[(*idx) + 1])
			return -1;

		(*idx)++;
		key.size = strtoull(search_keys[(*idx)], NULL, 10);
		(*idx)++;
	}

	/*
	 * NOT, OR, ()
	 */
	else if (strcasecmp(search_keys[*idx], "not") == 0) {
		key.type = IST_SUBSEARCH_NOT;

		(*idx)++;
		if ((result =
		     build_imap_search(search_keys, &key.sub_search,
				       idx, sorted )) < 0) {
			list_freelist(&key.sub_search.start);
			return result;
		}

		/* a NOT should be unary */
		if (key.sub_search.total_nodes != 1) {
			free_searchlist(&key.sub_search);
			return -1;
		}
	} else if (strcasecmp(search_keys[*idx], "or") == 0) {
		key.type = IST_SUBSEARCH_OR;

		(*idx)++;
		if ((result =
		     build_imap_search(search_keys, &key.sub_search,
				       idx, sorted)) < 0) {
			list_freelist(&key.sub_search.start);
			return result;
		}

		if ((result =
		     build_imap_search(search_keys, &key.sub_search,
				       idx, sorted )) < 0) {
			list_freelist(&key.sub_search.start);
			return result;
		}

		/* an OR should be binary */
		if (key.sub_search.total_nodes != 2) {
			free_searchlist(&key.sub_search);
			return -1;
		}

	} else if (strcasecmp(search_keys[*idx], "(") == 0) {
		key.type = IST_SUBSEARCH_AND;

		(*idx)++;
		while ((result =
			build_imap_search(search_keys, &key.sub_search,
					  idx, sorted)) == 0 && search_keys[*idx]);

		if (result < 0) {
			/* error */
			list_freelist(&key.sub_search.start);
			return result;
		}

		if (result == 0) {
			/* no ')' encountered (should not happen, parentheses are matched at the command line) */
			free_searchlist(&key.sub_search);
			return -1;
		}
	} else if (strcasecmp(search_keys[*idx], ")") == 0) {
		(*idx)++;
		return 1;
	} else if (check_msg_set(search_keys[*idx])) {
		key.type = IST_SET;
		strncpy(key.search, search_keys[*idx], MAX_SEARCH_LEN);
		(*idx)++;
	} else {
		/* unknown search key */
		return -1;
	}

	if (!list_nodeadd(sl, &key, sizeof(key)))
		return -2;

	return 0;
}


/*
 * perform_imap_search()
 *
 * returns 0 on succes, -1 on dbase error, -2 on memory error, 1 if result set is too small
 * (new mail has been added to mailbox while searching, mailbox data out of sync)
 */
int perform_imap_search(unsigned int *rset, int setlen, search_key_t * sk,
			mailbox_t * mb, int sorted)
{
	search_key_t *subsk;
	struct element *el;
	int result, i;
	unsigned int *newset = NULL;
	int subtype = IST_SUBSEARCH_OR;

	if (!rset)
		return -2;	/* stupidity */

	if (!sk)
		return 0;	/* no search */

	newset = (int *)g_malloc0(sizeof(int) * setlen);
	
	if (!newset)
		return -2;

	switch (sk->type) {
	case IST_SET:
		build_set(rset, setlen, sk->search);
		break;

	case IST_SET_UID:
		build_uid_set(rset, setlen, sk->search, mb);
		break;

	case IST_SORT:
		result = db_search(rset, setlen, sk->search, mb, sk->type);
		my_free(newset);
		return 0;
		break;

	case IST_SORTHDR:
		result = db_sort_parsed(rset, setlen, sk, mb);
		my_free(newset);
		return 0;
		break;

 
	case IST_FLAG:
		result = db_search(rset, setlen, sk->search, mb, sk->type);
		if (result != 0) {
			my_free(newset);
			return result;
		}
		break;

	case IST_HDR:
	case IST_HDRDATE_BEFORE:
	case IST_HDRDATE_ON:
	case IST_HDRDATE_SINCE:
	case IST_DATA_BODY:
	case IST_DATA_TEXT:
	case IST_SIZE_LARGER:
	case IST_SIZE_SMALLER:
		/* these all have in common that a message should be parsed before 
		   matching is possible
		 */
		result = db_search_parsed(rset, setlen, sk, mb);
		break;

	case IST_IDATE:
		result = db_search(rset, setlen, sk->search, mb, sk->type);
		if (result != 0) {
			my_free(newset);
			return result;
		}
		break;

	case IST_SUBSEARCH_NOT:
	case IST_SUBSEARCH_AND:
		subtype = IST_SUBSEARCH_AND;

	case IST_SUBSEARCH_OR:
		el = list_getstart(&sk->sub_search);
		while (el) {
			subsk = (search_key_t *) el->data;

			if (subsk->type == IST_SUBSEARCH_OR)
				memset(newset, 0, sizeof(int) * setlen);
			else if (subsk->type == IST_SUBSEARCH_AND
				 || subsk->type == IST_SUBSEARCH_NOT)
				for (i = 0; i < setlen; i++)
					newset[i] = 1;

			result =
			    perform_imap_search(newset, setlen, subsk, mb, sorted);
			if (result < 0 || result == 1) {
				my_free(newset);
				return result;
			}
			if (! sorted)
				combine_sets(rset, newset, setlen, subtype);
			else {
				for (i=0; i<setlen; i++)
					rset[i] = newset[i];
			}
 
			el = el->nextnode;
		}

		if (sk->type == IST_SUBSEARCH_NOT)
			invert_set(rset, setlen);

		break;

	default:
		my_free(newset);
		return -2;	/* ??? */
	}

	my_free(newset);
	return 0;
}


/*
 * frees the search-list sl
 *
 */
void free_searchlist(struct list *sl)
{
	search_key_t *sk;
	struct element *el;

	if (!sl)
		return;

	el = list_getstart(sl);

	while (el) {
		sk = (search_key_t *) el->data;

		free_searchlist(&sk->sub_search);
		list_freelist(&sk->sub_search.start);

		el = el->nextnode;
	}

	list_freelist(&sl->start);
	return;
}


void invert_set(unsigned int *set, int setlen)
{
	int i;

	if (!set)
		return;

	for (i = 0; i < setlen; i++)
		set[i] = !set[i];
}


void combine_sets(unsigned int *dest, unsigned int *sec, int setlen, int type)
{
	int i;

	if (!dest || !sec)
		return;

	if (type == IST_SUBSEARCH_AND) {
		for (i = 0; i < setlen; i++)
			dest[i] = (sec[i] && dest[i]);
	} else if (type == IST_SUBSEARCH_OR) {
		for (i = 0; i < setlen; i++)
			dest[i] = (sec[i] || dest[i]);
	}
}


/* 
 * build_set()
 *
 * builds a msn-set from a IMAP message set spec. the IMAP set is supposed to be correct,
 * no checks are performed.
 */
void build_set(unsigned int *set, unsigned int setlen, char *cset)
{
	unsigned int i;
	u64_t num, num2;
	char *sep = NULL;

	if (!set)
		return;

	memset(set, 0, setlen * sizeof(int));

	if (!cset)
		return;

	do {
		num = strtoull(cset, &sep, 10);

		if (num <= setlen && num > 0) {
			if (!*sep)
				set[num - 1] = 1;
			else if (*sep == ',') {
				set[num - 1] = 1;
				cset = sep + 1;
			} else {
				/* sep == ':' here */
				sep++;
				if (*sep == '*') {
					for (i = num - 1; i < setlen; i++)
						set[i] = 1;

					cset = sep + 1;
				} else {
					cset = sep;
					num2 = strtoull(cset, &sep, 10);

					if (num2 > setlen)
						num2 = setlen;
					if (num2 > 0) {
						/* NOTE: here: num2 > 0, num > 0 */
						if (num2 < num) {
							/* swap! */
							i = num;
							num = num2;
							num2 = i;
						}

						for (i = num - 1; i < num2;
						     i++)
							set[i] = 1;
					}
					if (*sep)
						cset = sep + 1;
				}
			}
		} else {
			/* invalid num, skip it */
			if (*sep) {
				cset = sep + 1;
				sep++;
			}
		}
	} while (sep && *sep && cset && *cset);
}


/* 
 * build_uid_set()
 *
 * as build_set() but takes uid's instead of MSN's
 */
void build_uid_set(unsigned int *set, unsigned int setlen, char *cset,
		   mailbox_t * mb)
{
	unsigned int i, msn, msn2;
	int result;
	int num2found = 0;
	u64_t num, num2;
	char *sep = NULL;

	if (!set)
		return;

	memset(set, 0, setlen * sizeof(int));

	if (!cset || setlen == 0)
		return;

	do {
		num = strtoull(cset, &sep, 10);
		result =
		    binary_search(mb->seq_list, mb->exists, num, &msn);

		if (result < 0 && num < mb->seq_list[mb->exists - 1]) {
			/* ok this num is not a UID, but if a range is specified (i.e. 1:*) 
			 * it is valid -> check *sep
			 */
			if (*sep == ':') {
				result = 1;
				for (msn = 0; mb->seq_list[msn] < num;
				     msn++);
				if (msn >= mb->exists)
					msn = mb->exists - 1;
			}
		}

		if (result >= 0) {
			if (!*sep)
				set[msn] = 1;
			else if (*sep == ',') {
				set[msn] = 1;
				cset = sep + 1;
			} else {
				/* sep == ':' here */
				sep++;
				if (*sep == '*') {
					for (i = msn; i < setlen; i++)
						set[i] = 1;

					cset = sep + 1;
				} else {
					/* fetch second number */
					cset = sep;
					num2 = strtoull(cset, &sep, 10);
					result =
					    binary_search(mb->seq_list,
							  mb->exists, num2,
							  &msn2);

					if (result < 0) {
						/* in a range: (like 1:1000) so this number doesnt need to exist;
						 * find the closest match below this UID value
						 */
						if (mb->exists == 0)
							num2found = 0;
						else {
							for (msn2 =
							     mb->exists -
							     1;; msn2--) {
								if (msn2 ==
								    0
								    && mb->
								    seq_list
								    [msn2]
								    > num2) {
									num2found
									    =
									    0;
									break;
								} else
								    if
								    (mb->
								     seq_list
								     [msn2]
								     <=
								     num2)
								{
									/* found! */
									num2found
									    =
									    1;
									break;
								}
							}
						}

					} else
						num2found = 1;

					if (num2found == 1) {
						if (msn2 < msn) {
							/* swap! */
							i = msn;
							msn = msn2;
							msn2 = i;
						}

						for (i = msn; i <= msn2;
						     i++)
							set[i] = 1;
					}

					if (*sep)
						cset = sep + 1;
				}
			}
		} else {
			/* invalid num, skip it */
			if (*sep) {
				cset = sep + 1;
				sep++;
			}
		}
	} while (sep && *sep && cset && *cset);
}


void dumpsearch(search_key_t * sk, int level)
{
	char *spaces = (char *) my_malloc(level * 3 + 1);
	struct element *el;
	search_key_t *subsk;

	if (!spaces)
		return;

	memset(spaces, ' ', level * 3);
	spaces[level * 3] = 0;

	if (!sk) {
		trace(TRACE_DEBUG, "%s(null)\n", spaces);
		my_free(spaces);
		return;
	}

	switch (sk->type) {
	case IST_SUBSEARCH_NOT:
		trace(TRACE_DEBUG, "%sNOT\n", spaces);

		el = list_getstart(&sk->sub_search);
		if (el)
			subsk = (search_key_t *) el->data;
		else
			subsk = NULL;

		dumpsearch(subsk, level + 1);
		break;

	case IST_SUBSEARCH_AND:
		trace(TRACE_DEBUG, "%sAND\n", spaces);
		el = list_getstart(&sk->sub_search);

		while (el) {
			subsk = (search_key_t *) el->data;
			dumpsearch(subsk, level + 1);

			el = el->nextnode;
		}
		break;

	case IST_SUBSEARCH_OR:
		trace(TRACE_DEBUG, "%sOR\n", spaces);
		el = list_getstart(&sk->sub_search);

		while (el) {
			subsk = (search_key_t *) el->data;
			dumpsearch(subsk, level + 1);

			el = el->nextnode;
		}
		break;

	default:
		trace(TRACE_DEBUG, "%s[type %d] \"%s\"\n", spaces,
		      sk->type, sk->search);
	}

	my_free(spaces);
	return;
}


/*
 * closes the msg cache
 */
void close_cache()
{
	if (cached_msg.msg_parsed)
		db_free_msg(&cached_msg.msg);

	cached_msg.num = -1;
	cached_msg.msg_parsed = 0;
	memset(&cached_msg.msg, 0, sizeof(cached_msg.msg));

	mclose(&cached_msg.memdump);
	mclose(&cached_msg.tmpdump);
}

/* 
 * init cache 
 */
int init_cache()
{
	cached_msg.num = -1;
	cached_msg.msg_parsed = 0;
	memset(&cached_msg.msg, 0, sizeof(cached_msg.msg));

	cached_msg.memdump = mopen();
	if (!cached_msg.memdump)
		return -1;

	cached_msg.tmpdump = mopen();
	if (!cached_msg.tmpdump) {
		mclose(&cached_msg.memdump);
		return -1;
	}

	cached_msg.file_dumped = 0;
	cached_msg.dumpsize = 0;
	return 0;
}

/* unwrap strings */
int mime_unwrap(char *to, const char *from) 
{
	while (*from) {
		if (((*from == '\n') || (*from == '\r')) && isspace(*(from+1))) {
			from+=2;
			continue;
		}
		*to++=*from++;
	}
	*to='\0';
	return 0;
}


