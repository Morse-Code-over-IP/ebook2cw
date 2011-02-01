/* 
ebook2cw - converts an ebook to morse mp3s

Copyright (C) 2007 - 2010  Fabian Kurz, DJ1YFK

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; either version 2 of the License, or (at your option) any later
version.

This program is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
PARTICULAR PURPOSE.  See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc., 51 Franklin
Street, Fifth Floor, Boston, MA  02110-1301, USA.

source code looks properly indented with ts=4

*/

#include <lame/lame.h>
#include <vorbis/vorbisenc.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <ctype.h>

/* for mkdir, not used on Windows */
#if !__MINGW32__
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#endif

#include "codetables.h"

#ifndef VERSION
#define VERSION "0.0.0"
#endif

#define PCMBUFFER   220500	/* 20 seconds at 11kHz, for one word. plenty.	*/
#define NOISEBUFFER 220500	/* 20 seconds at 11kHz, for one word. plenty.	*/
#define MP3BUFFER   285000    /* abt 1.25*PCMBUFFER + 7200, as recommended	*/

#define ISO8859 0
#define UTF8    1

#define MP3 0
#define OGG 1

#define SINE 0
#define SAWTOOTH 1
#define SQUARE 2

#define NOISEAMPLITUDE (10000.0)
#define CWAMPLITUDE (20000.0)

/* Global for LAME */
lame_global_flags *gfp;

/* Globals for OGG/Vorbis */
ogg_stream_state os;
ogg_page         og;
ogg_packet       op; 
vorbis_info      vi;
vorbis_comment   vc; 
vorbis_dsp_state vd; 
vorbis_block     vb;

/* Struct CWP to keep all CW parameters */

typedef struct {
	/* CW parameters */
	int wpm, 					/* speed, words per minute */
		freq, 					/* audio frequency in Hz */
		rt, 					/* risetime, in samples */
		ft, 					/* falltime, in samples */
		qrq,					/* increase speed each qrq minutes */
		reset, 					/* reset qrq each chapter? */
		farnsworth, 			/* effective, farnsworth speed */
		pBT, 					/* sent <BT> for each paragraph? */
		waveform, 				/* waveform, sine(1), sawtooth(2), square(3) */
		original_wpm,			/* starting speed; if qrq is used */
		wordspace,				/* wordspace in samples, depends on Farnsworth*/
		letterspace;			/* inter-letter space */
	float ews;					/* extra word space */
	/* Noise parameters */
	int bandpassbw,				/* Noise filter bandwidth */
		bandpassfc,				/* f_center of the bandpass */
		addnoise,				/* 1 if noise should be added, 0 if not */
		snr;					/* SNR of the CW with the noise */
	/* mp3/ogg encoder parameters, buffers */
	int encoder;			
	int samplerate,
		brate,
		quality;
	int inpcm_size,
		mp3buffer_size,
		noisebuf_size,
		ditlen,					/* normal dit length in samples */
		fditlen,				/* farnsworth ditlen in samples */
		maxbytes;				/* max bytes of a 'dah' */
	short int *dit_buf,
			*dah_buf,
			*inpcm,
			*noisebuf;
	unsigned char *mp3buffer;
	/* Chapter splitting */
	char chapterstr[80], 		/* split chapters by this string */
		 chapterfilename[80];
	/* encoding and mapping */
	int encoding,				/* ISO8859 or UTF8 */
		use_isomapping,
		use_utf8mapping;
	char isomapindex[256];		/* contains the chars to be mapped */
	int utf8mapindex[256];		/* for utf8 as decimal code */
	char isomap[256][4]; 		/* by these strings */
	char utf8map[256][8];

	char configfile[1025];

	char id3_author[80],
		id3_title[80],
		id3_comment[80],
		id3_year[5];

	FILE *outfile;
} CWP;


/* functions */
void  init_cw (CWP *cw);
void  init_encoder (CWP *cw);
void  encode_buffer (int length, CWP *cw);
void  ogg_encode_and_write (CWP *cw);
void  help (void);
void  showcodes (int i);
int   makeword(char * text, CWP *cw);
void  closefile (int letter, int chw, int chms, CWP *cw);
void  openfile (int chapter, CWP *cw);
void  buf_alloc (CWP *cw);
void  buf_check (int j, CWP *cw);
void  command (char * cmd, CWP *cw);
void  readconfig (CWP *cw);
void  setparameter (char p, char * value, CWP *cw);
void  loadmapping(char *filename, int enc, CWP *cw);
char  *mapstring (char *string, CWP *cw);
void  addnoise (int length, CWP *cw);
float snramplitude (int snr);
void  fillnoisebuffer (short int *buf, int size, float amplitude);
void  filterloop (short int *buf, int l, int b);
void  scalebuffer(short int *buf, int length, float factor);
void  addbuffer (short int *b1, short int *b2, int l);
char  *timestring (int ms);
void  guessencoding (char *filename);

#ifdef CGI
int   hexit (char c);
void  urldecode(char *buf);
#endif

/* main */

int main (int argc, char** argv) {
	int pos, i, c, tmp;
 	char word[1024]="";				/* will be cut when > 1024 chars long */
	int chapter = 0;
	int chw = 0, tw = 0, qw = 0;	/* chapter words, total words, qrq words */
	int chms = 0, tms = 0, qms = 0; /* millisec: chapter, total, since qrq */
	FILE *infile;

#ifdef CGI
	char * querystring;
	static char text[10000];
#endif

	/* initializing the CW parameter struct with standard values */
	CWP cw;
	cw.wpm = 25;
	cw.freq = 600;
	cw.rt = 50;
	cw.ft = 50;
	cw.qrq = 0;
	cw.reset = 1;
	cw.farnsworth = 0;
	cw.ews = 0.0;
	cw.pBT = 1;
	cw.waveform = SINE;
	
	cw.bandpassbw = 500;
	cw.bandpassfc = 800;
	cw.addnoise = 0;
	cw.snr = 0;

	cw.encoder = MP3;

	cw.samplerate = 11025;
	cw.brate = 16;
	cw.quality = 5;
	
	cw.inpcm_size = PCMBUFFER;
	cw.noisebuf_size = NOISEBUFFER;
	cw.mp3buffer_size = MP3BUFFER;
	cw.ditlen = 0;
	strcpy(cw.chapterstr, "CHAPTER");
	strcpy(cw.chapterfilename, "Chapter");

	cw.encoding = ISO8859;
	cw.use_isomapping = cw.use_utf8mapping = 0;

	strcpy(cw.configfile, "ebook2cw.conf");

	strcpy(cw.id3_author, "CW audio book");
	strcpy(cw.id3_title, "");
	strcpy(cw.id3_comment, "");
	strcpy(cw.id3_year, "");

	infile = stdin;

#ifndef CGI

	printf("ebook2cw %s - (c) 2011 by Fabian Kurz, DJ1YFK\n\n", VERSION);

	/* 
	 * Find and read ebook2cw.conf 
	 */

	readconfig(&cw);

	while((i=getopt(argc,argv, "Oo:w:W:e:f:uc:k:Q:R:pF:s:b:q:a:t:y:S:hnT:N:B:C:g:"))!= -1){
		setparameter(i, optarg, &cw);
	} 

	if (optind < argc) {		/* something left? if so, use as infile */
		if ((argv[optind][0] != '-') && (argv[optind][0] != '\0')) {
			if ((infile = fopen(argv[optind], "r")) == NULL) {
				fprintf(stderr, "Error: Cannot open file %s. Exit.\n", 
								argv[optind]);
				exit(EXIT_FAILURE);
			}
		}
	}
#endif	/* ifndef CGI */

	/* init encoder (LAME or OGG/Vorbis) */
	init_encoder(&cw);

	/* Initially allocate inpcm, noisebuffer, mp3buffer, dit_buf, dah_buf  */
	buf_alloc(&cw);

#ifndef CGI
	printf("Speed: %dwpm, Freq: %dHz, Chapter: >%s<, Encoding: %s\n", cw.wpm, 
		cw.freq, cw.chapterstr, cw.encoding == UTF8 ? "UTF-8" : "ISO 8859-1");

	printf("Effective speed: %dwpm, ", cw.farnsworth ? cw.farnsworth : cw.wpm);
	printf("Extra word spaces: %1.1f, ", cw.ews);
	printf("QRQ: %dmin, reset QRQ: %s\n\n", cw.qrq, cw.reset ? "yes" : "no");
#endif


#ifdef CGI
	cw.outfile = stdout;
	switch (cw.encoder) {
		case MP3:
			printf("Content-type: audio/mpeg\n\n");
			break;
		case OGG:
			printf("Content-type: audio/ogg\n\n");
			break;
	}
	querystring = getenv("QUERY_STRING");
	if ((querystring == NULL) || strlen(querystring) > 9000) {
			exit(1);
	}
	sscanf(querystring, "s=%d&e=%d&f=%d&t=%9000s", &cw.wpm, &cw.farnsworth, &cw.freq, text);
	strcat(text, " ");
	urldecode(text);
#endif

	init_cw(&cw);	/* generate raw dit, dah */

	strcat(cw.chapterstr, " ");

	/* read STDIN, assemble full words (anything ending in ' ') to 'word' and
	 * generate CW, write to file by 'makeword'. words with > 1024 characters
	 * will be split  */

	cw.original_wpm = cw.wpm;					/* may be changed by QRQing */
	chapter = 0;
#ifndef CGI
	openfile(chapter, &cw);
#endif

	i=0;
	pos=0;

#ifndef CGI
	while ((c = getc(infile)) != EOF) {
#else
	while ((c = text[i++]) != '\0') {
#endif

		if (c == '\r') 			/* DOS linebreaks */
				continue;

		word[pos++] = c;
		
		if ((c == ' ') || (c == '\n') || (pos == 1024)) {
			word[pos] = '\0';
#ifndef CGI
			if (strcmp(cw.chapterstr, word) == 0) {	/* new chapter */
				closefile(chapter, chw, chms, &cw);
				tw += chw;
				tms += chms;
				chw = 0;
				chapter++;
		
				if (cw.qrq && cw.reset) {	
					cw.wpm = cw.original_wpm;	
					init_cw(&cw);
					qw = 0;
				}
					
				openfile(chapter, &cw);
			}
#endif

			/* check for commands: |f or |w */
			if (word[0] == '|') {
				command(word, &cw);
			}
			else {
				tmp = makeword(mapstring(word, &cw), &cw);
				chms += tmp;
				qms += tmp;
				chw++; 
			}

			word[0] = '\0';
			pos = 0;

			/* Every 'cw.qrq' minutes speed up 1 WpM */
			if (cw.qrq && ((qms/60000.0) > cw.qrq)) {
				cw.wpm += 1;
				init_cw(&cw);
				printf("QRQ: %dwpm\n", cw.wpm);
				qms = 0;
			}

		} /* word */

	} /* eof */
#ifndef CGI
	closefile(chapter, chw, chms, &cw);
	printf("Total words: %d, total time: %s\n", tw+chw, timestring(tms+chms));
#else
	if (cw.encoder == OGG) {
		vorbis_analysis_wrote(&vd,0);
		ogg_encode_and_write(&cw);
	}
#endif

	if (cw.encoder == MP3) {
		lame_close(gfp);
	}
	else {
		ogg_stream_clear(&os);
		vorbis_block_clear(&vb);
		vorbis_dsp_clear(&vd);
		vorbis_comment_clear(&vc);
		vorbis_info_clear(&vi);
	}
	
	free(cw.mp3buffer);
	free(cw.inpcm);
	free(cw.noisebuf);

	return (EXIT_SUCCESS);
}





/* init_cw  -  generates a dit and a dah to dit_buf and dah_buf */

void init_cw (CWP *cw) {
	int x, len;
	double val, val1, val2;

	/* calculate farnsworth length samples */
	if (cw->farnsworth) {
		if (cw->farnsworth > cw->wpm) {
			fprintf(stderr, "Warning: Effective speed (-e %d) must be lower "
							"than character speed (-w %d)! Speed adjusted.\n", 
							cw->farnsworth, cw->wpm);
			cw->farnsworth = cw->wpm;
		}
		cw->fditlen = (int) (6.0*cw->samplerate/(5.0*cw->farnsworth));
	}

	/* dit */
	len = (int) (6.0*cw->samplerate/(5.0*cw->wpm));			/* in samples */

	/* size of dit_buf, dah_buf may have to be increased, when speed decreased */
	if (len > cw->ditlen) {
		if ((cw->dah_buf = realloc(cw->dah_buf, 3*len * sizeof(short int))) == NULL) {
			fprintf(stderr, "Error: Can't reallocate dah_buf[%d]\n", 3*len);
			exit(EXIT_FAILURE);
		}
		if ((cw->dit_buf = realloc(cw->dit_buf, len * sizeof(short int))) == NULL) {
			fprintf(stderr, "Error: Can't allocate dit_buf[%d]\n", len);
			exit(EXIT_FAILURE);
		}
	}

	/* both dah_buf and dit_buf are filled in this loop */
	for (x=0; x < 3*len; x++) {
		switch (cw->waveform) {
			case SINE:
				val = sin(2*M_PI*cw->freq*x/cw->samplerate);
				break;
			case SAWTOOTH:
				val = ((1.0*cw->freq*x/cw->samplerate)-
								floor(1.0*cw->freq*x/cw->samplerate))-0.5;
				break;
			case SQUARE:
				val = ceil(sin(2*M_PI*cw->freq*x/cw->samplerate))-0.5;
				break;
		}

		/* Shaping rising edge, same for dit and dah */
		if (x < cw->rt)
				val *= pow(sin(M_PI*x/(2.0*cw->rt)),2);

		val1 = val2 = val;

		/* Shaping falling edge, dit */
		if (x < len) {
		   if (x > (len-cw->ft)) {
				val1 *= pow((sin(2*M_PI*(x-(len-cw->ft)+cw->ft)/(4*cw->ft))), 2);
			}
			cw->dit_buf[x] = (short int) (val1 * CWAMPLITUDE);
		}

		/* Shaping falling edge, dah */
		if (x > (3*len-cw->ft)) {
				val2 *= pow((sin(2*M_PI*(x-(3*len-cw->ft)+cw->ft)/(4*cw->ft))), 2);
		}
		cw->dah_buf[x] = (short int) (val2 * CWAMPLITUDE);
	} /* for */
	
	cw->ditlen = len;

	/* Calculate maximum number of bytes per 'dah' for the PCM stream,
	 * this will be used later to check if the buffer runs full
	 * (10000 margin) */

	if (cw->farnsworth) {
		cw->maxbytes = (int) ((4+7.0*cw->ews)*(cw->fditlen)) + 10000;
	}
	else {	
		cw->maxbytes = (int) ((6+7.0*cw->ews) * len) + 10000;
	}

	/* Calculate word space, letter space, considering EWS and farnsworth */
	cw->wordspace =(int)(1+7*cw->ews)*(cw->farnsworth ? cw->fditlen : cw->ditlen);
	cw->letterspace = (cw->farnsworth ? 3*cw->fditlen - cw->ditlen : 2*cw->ditlen);

	return;
}





/* makeword  --  converts 'text' to CW by concatenating dit_buf amd dah_bufs,
 * encodes this to MP3 and writes to open filehandle outfile */

int makeword(char * text, CWP *cw) {
 const char *code;				/* CW code as . and - */

 int c, i, j, u, w;
 int prosign = 0;
 unsigned char last=0;		/* for utf8 2-byte characters */

 j = 0;						/* position in 'inpcm' buffer */

 for (i = 0; i < strlen(text); i++) {
	c = (unsigned char) text[i];
	code = NULL;

	if (c == '\n') { 				/* Same for UTF8 and ISO8859 */
		if ((strlen(text) == 1) && cw->pBT) 	/* paragraph */
			code = " -...- "; 
		else 
			code = " ";
	}
	else if (c == '<') {				/* prosign on */
		prosign = 1;
		continue;
	}
	else if ((c == '>') && prosign) {	/* prosign off */
		prosign = 0;
		code = "";			/* only inserts letter space */
	}
	else if (cw->encoding == ISO8859) {	
		code = iso8859[c];
	}
	else if (cw->encoding == UTF8) {
		/* Character may be 1-byte ASCII or 2-byte UTF8 */
		if (!(c & 128)) {						/* MSB = 0 -> 7bit ASCII */
			code = iso8859[c];					/* subset of iso8859 */
		}
		else {
			if (last && (c < 192) ) {			/* this is the 2nd byte */
				/* 110yyyyy 10zzzzzz -> 00000yyy yyzzzzzz */
				c = ((last & 31) << 6) | (c & 63);
				code = utf8table[c];
				last = 0;
			}
			else {								/* this is the first byte */
				last = c;
			}
		}
	}

	if (last) continue;				/* first of two-byte character read */

	/* Not found anything ... */
	if (code == NULL) {
		code = " ";
		if (c < 255) {
			fprintf(stderr, "Warning: don't know CW for '%c'\n", c);
		}
		else {
			fprintf(stderr, "Warning: don't know CW for unicode &#%d;\n", c);
		}

	}

	/* code contains letter as ./-, now assemble pcm buffer */

	for (w = 0; w < strlen(code) ; w++) {
	
		/* make sure the inpcm buffer doesn't run full,
		 * with a conservative margin. reallocate memory if neccesary */
		buf_check(j, cw);

		c = code[w];

		if (c == '.') 
			for (u=0; u < cw->ditlen; u++)
					cw->inpcm[++j] = cw->dit_buf[u]; 
		else if (c == '-') 
			for (u=0; u < (3*cw->ditlen); u++)
					cw->inpcm[++j] = cw->dah_buf[u]; 
		else 								/* word space */
			for (u=0;u < cw->wordspace; u++)
					cw->inpcm[++j] = 0; 
	
		for (u=0; u < cw->ditlen; u++)
				cw->inpcm[++j] = 0; 
	}	/* foreach dot/dash */

	if (prosign == 0) {
		for (u=0; u < cw->letterspace; u++)
			cw->inpcm[++j] = 0; 
	}

 }	/* foreach letter */

 /* j = total length of samples in 'inpcm' */

 if (cw->addnoise) {
	addnoise(j, cw);	
 }


 encode_buffer(j, cw); 

 return (int) (1000.0*j/cw->samplerate);

}


/* closefile -- finishes writing the current file, flushes the encoder buffer */

void closefile (int chapter, int chw, int chms, CWP *cw) {
	int outbytes;
	char outfilename[80] = "";

	printf("words: %d, time: %s\n", chw, timestring(chms));
	

	snprintf(outfilename, 80, "%s%04d.%s",  cw->chapterfilename, chapter, 
			(cw->encoder == MP3) ? "mp3" : "ogg");
	printf("Finishing %s\n\n",  outfilename);

	switch (cw->encoder) {
		case MP3:
			outbytes = lame_encode_flush(gfp,cw->mp3buffer, cw->mp3buffer_size);
 
			if (outbytes < 0) {
				fprintf(stderr, "Error: lame_encode_buffer returned %d.\n", 
					outbytes);
				exit(EXIT_FAILURE);
			}

			if (fwrite(cw->mp3buffer, sizeof(char), outbytes, cw->outfile) != 
					 outbytes) {
				fprintf(stderr, "Error: Writing %db to file failed. Exit.\n", 
						outbytes);
				exit(EXIT_FAILURE);
			}
			break;
		case OGG:
			vorbis_analysis_wrote(&vd,0);
			ogg_encode_and_write(cw);
			break;

	}

 fclose(cw->outfile);
}


/* openfile -- starts a new chapter by opening a new file as outfile */

void openfile (int chapter, CWP *cw) {
	char outfilename[80] = "";
	static char tmp[80] = "";
	ogg_packet       hdr;
	ogg_packet       hdr_comm;
	ogg_packet       hdr_code;

	snprintf(outfilename, 80, "%s%04d.%s",  cw->chapterfilename, chapter, 
			(cw->encoder == MP3) ? "mp3" : "ogg");
	printf("Starting %s\n",  outfilename);
 
	if ((cw->outfile = fopen(outfilename, "wb")) == NULL) {
		fprintf(stderr, "Error: Failed to open %s\n", outfilename);
		exit(EXIT_FAILURE);
	}

	switch (cw->encoder) {
		case MP3:
			snprintf(tmp, 79, "%s - %d", cw->id3_title, chapter);	/* title */
			id3tag_init(gfp);
			id3tag_set_artist(gfp, cw->id3_author);
			id3tag_set_year(gfp, cw->id3_year);
			id3tag_set_title(gfp, tmp);
			id3tag_set_comment(gfp, cw->id3_comment);
			break;
		case OGG:
			vorbis_comment_init(&vc);
			vorbis_comment_add_tag(&vc,"ENCODER","ebook2cw");
			vorbis_analysis_init(&vd, &vi);
			vorbis_block_init(&vd, &vb);
			ogg_stream_init(&os,rand());

			vorbis_analysis_headerout(&vd,&vc,&hdr,&hdr_comm,&hdr_code);
			ogg_stream_packetin(&os,&hdr); 
			ogg_stream_packetin(&os,&hdr_comm);
			ogg_stream_packetin(&os,&hdr_code);
			while (ogg_stream_flush(&os,&og)) {
				fwrite(og.header,1,og.header_len,cw->outfile);
				fwrite(og.body,1,og.body_len,cw->outfile);
			}
			break;
	}

}


void help (void) {
	printf("This is free software, and you are welcome to redistribute it\n"); 
	printf("under certain conditions (see COPYING).\n");
	printf("\n");
	printf("ebook2cw [-w wpm] [-f freq] [-R risetime] [-F falltime]\n");
	printf("         [-s samplerate] [-b mp3bitrate] [-q mp3quality] [-p]\n");
	printf("         [-c chapter-separator] [-o outfile-name] [-Q minutes]\n");
	printf("         [-a author] [-t title] [-k comment] [-y year]\n");
	printf("         [-u] [-S ISO|UTF] [-n] [-e eff.wpm] [-W space]\n");
	printf("         [-N snr] [-B filter bandwidth] [-C filter center]\n");
	printf("         [-T 1..3] [-g filename]\n");
	printf("         [infile]\n\n");
	printf("defaults: 25 WpM, 600Hz, RT=FT=50, s=11025Hz, b=16kbps,\n");
	printf("          c=\"CHAPTER\", o=\"Chapter\" infile = stdin\n");
	printf("\n");
	printf("Project website: http://fkurz.net/ham/ebook2cw.html\n");
	exit(EXIT_SUCCESS);
}


/*  
 *  showcodes
 *  Generates a HTML table of the available CW symbols in codetables.h
 *	i = 1 -> ISO 8859-1, i=0 -> UTF-8
 */

void showcodes (int i) {
	int k;
	int n = i ? 256 : 1921;

	printf("<html><head><META HTTP-EQUIV=\"CONTENT-TYPE\" CONTENT=\"text/html;"
		 	"charset=%s\"></head>\n", i ? "iso-8859-1" : "utf-8");
	printf("<body><h1>ebook2cw codetables.h</h1>\n");
	printf("<h2>%s</h2><table border=\"1\">\n", i ? "ISO 8859-1" : "UTF-8");
	printf("<tr><th>Decimal</th><th>Symbol</th><th>CW</th></tr>\n");

	for (k=0; k < n; k++) {
		printf("<tr>");

		if ((k < 128+i*128) && (iso8859[k] != NULL)) {
			printf("<td>%d</td><td>&#%d;</td><td>%s</td>", k, k, iso8859[k]); 
		}
		else if (utf8table[k] != NULL) {
			printf("<td>%d</td><td>&#%d;</td><td>%s</td>", k, k, utf8table[k]); 
		}
		else {
			printf("<td>%d</td><td>&#%d;</td><td>&nbsp;</td>", k, k); 
		}
	
		printf("</tr>\n");
	}
	
	printf("</table></body></html>\n");

	exit(EXIT_SUCCESS);
}



/* make sure the inpcm-buffer doesn't run full.
 * has to consider the effects of Farnsworth and extra word spacing 
 * since memory is cheap and cpu cycles are precious, the size of the 
 * buffers is doubled each time they run full (if they ever do)
 */

void buf_check (int j, CWP *cw) {

	if (j > cw->inpcm_size - cw->maxbytes) {
			cw->inpcm_size *=  2;
			cw->noisebuf_size = cw->inpcm_size;
			cw->mp3buffer_size *= 2;
			if ((cw->inpcm = realloc(cw->inpcm, cw->inpcm_size*sizeof(short int)))== NULL) {
				fprintf(stderr, "Error: Can't realloc inpcm[%d]\n", cw->inpcm_size);
				exit(EXIT_FAILURE);
			}
			
			if ((cw->noisebuf = realloc(cw->noisebuf, cw->noisebuf_size*sizeof(short int)))== NULL) {
				fprintf(stderr, "Error: Can't realloc noisebuf[%d]\n", cw->noisebuf_size);
				exit(EXIT_FAILURE);
			}
			fillnoisebuffer(cw->noisebuf, cw->noisebuf_size, NOISEAMPLITUDE);
			
			if ((cw->mp3buffer = realloc(cw->mp3buffer, cw->mp3buffer_size*sizeof(char))) 
						   	== NULL) {
				fprintf(stderr, "Error: Can't realloc mp3buffer[%d]\n", 
								cw->mp3buffer_size);
				exit(EXIT_FAILURE);
			}
		}

}

/* command
 * Receives a 'word' which starts with a | and then (possibly) a command,
 * to change speed or tone freq.
 */

void command (char * cmd, CWP *cw) {

	int i = 0;
	unsigned char c = 0;

	sscanf(cmd, "|%c%d", &c, &i);

	switch (c) {
		case 'f':
			if ((i > 100) && (i < (int) cw->samplerate/2)) {
				cw->freq = i;
				init_cw(cw);
			}
			else 
				fprintf(stderr, "Invalid frequency: %s. Ignored.\n", cmd);
			break;
		case 'w':
			if (i > 1) {
				cw->wpm = i;
				init_cw(cw);
			}
			else 
				fprintf(stderr, "Invalid speed: %s. Ignored.\n", cmd);
			break;
		case 'e':
			if (i > 1) {
				cw->farnsworth = i;
				init_cw(cw);
			}
			else 
				fprintf(stderr, "Invalid speed: %s. Ignored.\n", cmd);
			break;
		case 'T':
			if (i > 0 && i < 3) {
				cw->waveform = i;
				init_cw(cw);
			}
			else 
				fprintf(stderr, "Invalid waveform: %d. Ignored.\n", i);
			break;
		case 'N':
			cw->addnoise = 1;
			cw->snr = i;
			break;
		default:
			fprintf(stderr, "Invalid command %s. Ignored.\n", cmd);				
	}
}

/* readconfig
 *
 * reads a ebook2cw.conf file and sets parameters from the [settings]
 * part, then looks for character mappings in the [mappings] part.
 *
 * ebook2cw.conf is first searched in the current directory, then
 * ~/.ebook2cw/ and finally in DESTDIR/share/doc/ebook2cw/examples/
 * and copied to ~/.ebook2cw/ if needed.
 *
 * A sample config `ebook2cw.conf' is included.
 */

void readconfig (CWP *cw) {
	FILE *conf = NULL;
	char tmp[1024] = "";
	char p;					/* parameter */
	char v[80]="";			/* value */
	static char mapfile[1024]="";
	char *homedir = NULL;
	int j=0;

	if ((conf = fopen(cw->configfile, "r")) == NULL) {
		/* ebook2cw.conf not in current directory */
		if ((homedir = getenv("HOME")) == NULL) {	/* no home or Windows */
			return;
		}
#if !__MINGW32__
		/* Linux/Unix: Look for ~/.ebook2cw/ebook2cw.conf */
		snprintf(cw->configfile, 2048, "%s/.ebook2cw/ebook2cw.conf", homedir);
		if ((conf = fopen(cw->configfile, "r")) == NULL) {
			/* Not in ~/.ebook2cw/ either, look for it in
			 * DESTDIR/ebook2cw/examples/ */
			if ((conf = fopen(DESTDIR
				"/share/doc/ebook2cw/examples/ebook2cw.conf","r")) == NULL) {
				/* cannot find ebook2cw.conf anywhere. silently return */
				return;
			}
			else {
				printf("First run. Copying example configuration files to "
					"%s/.ebook2cw/\n\n", homedir);
				snprintf(tmp, 1024, "%s/.ebook2cw/", homedir);
				j = mkdir(tmp, 0777);
				if (j && (errno != EEXIST)) {
					printf("Failed to create %s. Resuming without config.",tmp);
					return;
				}
				/* ~/.ebook2cw/ created. Now copy ebook2cw.conf and map files */
				snprintf(tmp, 1024, "install -m 644 "DESTDIR
					"/share/doc/ebook2cw/examples/ebook2cw.conf %s/.ebook2cw/",
					homedir);
				if (system(tmp)) {
					printf("Failed to create ~/.ebook2cw/ebook2cw.conf. "
									"Resuming without config.");
					return;
				}

				snprintf(tmp, 1024, "install -m 644 "DESTDIR
					"/share/doc/ebook2cw/examples/isomap.txt %s/.ebook2cw/",
					homedir);
				if (system(tmp)) {
					printf("Failed to create ~/.ebook2cw/isomap.txt. "
									"Resuming without config.");
					return;
				}

				snprintf(tmp, 1024, "install -m 644 "DESTDIR
					"/share/doc/ebook2cw/examples/utf8map.txt %s/.ebook2cw/",
					homedir);
				if (system(tmp)) {
					printf("Failed to create ~/.ebook2cw/utf8map.txt. "
									"Resuming without config.");
					return;
				}

				/* Succcess. files installed to ~/.ebook2cw/ */

				snprintf(cw->configfile, 1024, "%s/.ebook2cw/ebook2cw.conf",
								homedir);

				/* open newly installed config file ... */
				if ((conf = fopen(cw->configfile, "r")) == NULL) {
					printf("Couldn't read %s! Continue without config.",
									cw->configfile);
					return;
				}

			}
		}
#endif
	}

	printf("Reading configuration file: %s\n\n", cw->configfile);

	/* We start in the [settings] section.
	 * All settings are ^[a-zA-Z]=.+$ 
	 * */

	while ((feof(conf) == 0) && (fgets(tmp, 80, conf) != NULL)) {
		tmp[strlen(tmp)-1]='\0';
		if (strstr(tmp, "[mappings]")) {		/* all parameters read */
			break;
		}
		else {
			if (sscanf(tmp, "%c=%s", &p, v) == 2) {
				setparameter(p, v, cw);
			}
		}
	}

	/* mappings */


	while ((feof(conf) == 0) && (fgets(tmp, 80, conf) != NULL)) {
		tmp[strlen(tmp)-1]='\0';
		if (sscanf(tmp, "isomapfile=%255s", mapfile) == 1) {
				loadmapping(mapfile, ISO8859, cw);
		}
		else if (sscanf(tmp, "utf8mapfile=%255s", mapfile) == 1) {
				loadmapping(mapfile, UTF8, cw);
		}
	}
}


void setparameter (char i, char *value, CWP *cw) {

		switch (i) {
			case 'w':
				if ((cw->wpm = atoi(value)) < 1) {
					fprintf(stderr, "Error: Speed (-w) must be > 0!\n");
					exit(EXIT_FAILURE);
				}
				break;
			case 'f':
				cw->freq = atoi(value);
				break;
			case 'R':
				cw->rt = atoi(value);
				break;
			case 'F':
				cw->ft = atoi(value);
				break;
			case 's':
				cw->samplerate = atoi(value);
				break;
			case 'b':
				cw->brate = atoi(value);
				break;
			case 'q':
				cw->quality = atoi(value);
				break;
			case 'c':
				strncpy(cw->chapterstr, value, 78);
				break;
			case 'o':
				strncpy(cw->chapterfilename, value, 78);
				break;
			case 'a':
				strncpy(cw->id3_author, value, 78);
				break;
			case 't':
				strncpy(cw->id3_title, value, 75);
				break;
			case 'k':
				strncpy(cw->id3_comment, value, 78);
				break;
			case 'y':
				strncpy(cw->id3_year, value, 4);
				break;
			case 'Q':
				if ((cw->qrq = atoi(value)) < 1) {
					fprintf(stderr, "Error: QRQ time (-Q) must be > 0!\n");
					exit(EXIT_FAILURE);
				}
				break;
			case 'u':
				cw->encoding = UTF8;
				break;
			case 'S':
				if (strstr(value, "ISO")) {
					showcodes(1);
				}
				else {
					showcodes(0);
				}
				break;
			case 'e':				/* effective speed in WpM */
				if ((cw->farnsworth = atoi(value)) < 1) {
					fprintf(stderr, "Error: Eff. speed (-e) must be > 0!\n");
					exit(EXIT_FAILURE);
				}
				break;
			case 'W':
				cw->ews = atof(value);
				break;
			case 'n':
				cw->reset = 0;
				break;
			case 'O':
				cw->encoder = OGG;
				break;
			case 'p':
				cw->pBT = 0;
				break;
			case 'g':
				guessencoding(value);
				break;
			case 'h':
				help();
				break;
			case 'T':
				if (strstr(value, "SINE") || atoi(value) == 1) {
					cw->waveform = SINE;
				}
				else if (strstr(value, "SAWTOOTH") || atoi(value) == 2) {
					cw->waveform = SAWTOOTH;
				}
				else if (strstr(value, "SQUARE") || atoi(value) == 3) {
					cw->waveform = SQUARE;	
				}
				break;
			case 'N':
				cw->snr = atoi(value);
				cw->addnoise = 1;
				if ((cw->snr < -10) || (cw->snr > 10)) {
					fprintf(stderr, "Warning: SNR %ddB not implemented."
									" Noise disabled.\n\n", cw->snr);
					cw->addnoise = 0;
					cw->snr = 0;
				}
				break;
			case 'B':
				cw->bandpassbw = atoi(value);
				break;
			case 'C':
				cw->bandpassfc = atoi(value);
				break;
				
		} /* switch */

}

/* loadmapping
 *
 * opens a mapping file and writes the mappings to the mapping variables:
 * index:     utf8mapindex, isomapindex
 * mappings:  utf8map, isomap
 *
 * */


void loadmapping(char *file, int enc, CWP *cw) {
	FILE *mp;
	char tmp[81] = "";
	int i=0;
	int k=0;
	char c1=0;
	char c2=0;
	char s[6]="";

	if ((mp = fopen(file, "r")) == NULL) {
		fprintf(stderr, "Warning: Unable to open mapping file %s. Ignored.\n", 
								file);
		return;
	}

	switch (enc) {
			case ISO8859:
				memset(cw->isomapindex, 0, sizeof(char)*256);
				cw->use_isomapping = 1;
				break;
			case UTF8:
				memset(cw->utf8mapindex, 0, sizeof(int)*256);
				cw->use_utf8mapping = 1;
				break;
	}


	while ((feof(mp) == 0) && (fgets(tmp, 80, mp) != NULL)) {
		tmp[strlen(tmp)-1]='\0';

		switch (enc) {
			case ISO8859:
				if (sscanf(tmp, "%c=%3s", &c1, s) == 2) {
					cw->isomapindex[i] = c1;
					strncpy(cw->isomap[i], s, 3);
					i++;
				}	
				break;
			case UTF8:
				if (sscanf(tmp, "%c=%5s", &c1, s) == 2) {
						cw->utf8mapindex[i] = c1;
						strncpy(cw->utf8map[i], s, 5);
						i++;
				}
				else if (sscanf(tmp, "%c%c=%5s", &c1, &c2, s) == 3) {
						k = ((c1 & 31) << 6) | (c2 & 63);	/* decimal */
						cw->utf8mapindex[i] = k;			/* unicode char */
						strncpy(cw->utf8map[i], s, 5);
						i++;
				}
				break;
		}

		/* only memory for 256 mappings allocated. never going to happen */
		if (i == 255) {
			fprintf(stderr, "Warning: Maximum number (256) of mappings reached"
							" in %s! Stopped here.", file);
			break;
		}

	} /* while */

}


/* mapstring
 * receives a string and replaces all characters that show up in isomapindex /
 * utf8mapindex with their replacement strings, and returns it.
 */


char *mapstring (char * string, CWP *cw) {
	static char new[2048]="";
	char c;
	int i, j, replaced;
	unsigned char last=0;

	memset(new, 0, 2048);
	
	switch (cw->encoding) {
		case ISO8859:
			if (!cw->use_isomapping) {
				return string;
			}
			while ((c = *string++) != '\0') {
				replaced = 0;
				for (i=0; i < 255; i++) {
					if (cw->isomapindex[i] == 0) {
						break;
					}
					else if (cw->isomapindex[i] == c) {
						strcat(new, cw->isomap[i]);
						replaced = 1;
						break;
					}
				}
				if (!replaced) {
					new[strlen(new)] = c;
				}
			}
		break;
		case UTF8:
			if (!cw->use_utf8mapping) {
				return string;
			}
			while ((c = *string++) != '\0') {
				j = 0;
				replaced = 0;
				if (!last && (c & 128)) {		/* first of a 2 char seq */
					last = c;
					continue;
				}
				if (last) {	
					/* 110yyyyy 10zzzzzz -> 00000yyy yyzzzzzz */
					j = ((last & 31) << 6) | (c & 63);
				}
				else {
					j = c;
				}
				for (i=0; i < 255; i++) {
					if (cw->utf8mapindex[i] == 0) {
						break;
					}
					else if (cw->utf8mapindex[i] == j) {
						strcat(new, cw->utf8map[i]);
						replaced = 1;
						break;
					}
				}
				if (!replaced) {
					if (last) {
						new[strlen(new)] = last;
						new[strlen(new)] = c;
					}
					else {
						new[strlen(new)] = c;
					}
				}
				last = 0;
			}
	}

	return new;

}



void buf_alloc(CWP *cw) {

	/* pcm, noise and mp3 buffers will be increased later as needed, but the
	 * initial values should be sufficient for most speeds and reasonably long
	 * words */
	if ((cw->inpcm = calloc(PCMBUFFER, sizeof(short int))) == NULL) {
		fprintf(stderr, "Error: Can't allocate inpcm[%d]!\n", PCMBUFFER);
		exit(EXIT_FAILURE);
	}
	cw->inpcm_size = PCMBUFFER;

	if ((cw->noisebuf = calloc(NOISEBUFFER, sizeof(short int))) == NULL) {
		fprintf(stderr, "Error: Can't allocate noisebuf[%d]!\n", NOISEBUFFER);
		exit(EXIT_FAILURE);
	}
	cw->noisebuf_size = NOISEBUFFER;
	fillnoisebuffer(cw->noisebuf, cw->noisebuf_size, NOISEAMPLITUDE);

	if ((cw->mp3buffer = calloc(MP3BUFFER, sizeof(unsigned char))) == NULL) {
		fprintf(stderr, "Error: Can't allocate mp3buffer[%d]!\n", MP3BUFFER);
		exit(EXIT_FAILURE);
	}
	cw->mp3buffer_size = MP3BUFFER;

	/* Just give it one byte now. It will be reallocated everytime init_cw is
	 * called, but to make it easy, allocate *something* to it now */
	if ((cw->dah_buf = calloc(1, sizeof(short int))) == NULL) {
		fprintf(stderr, "Error: Can't allocate dah_buf[1]!\n");
		exit(EXIT_FAILURE);
	}

	if ((cw->dit_buf = calloc(1, sizeof(short int))) == NULL) {
		fprintf(stderr, "Error: Can't allocate dit_buf[1]!\n");
		exit(EXIT_FAILURE);
	}

}

#ifdef CGI

/* 
 * URLDECODE stuff, needed for the CGI only:
 * */

/* Anti-Web HTTPD */
/* Hardcore Software */
/*
This software is Copyright (C) 2001-2004 By Hardcore Software and
others. The software is distributed under the terms of the GNU General
Public License. See the file 'COPYING' for more details.
*/

/*
This code is Copyright (C) 2001 By Zas ( zas at norz.org )
The software is distributed under the terms of the GNU General Public
License. See the file 'COPYING' for more details.
*/

/* I might've modified this slightly, but it's definitley Zas'. -Fractal */

int hexit(char c) {
    if ( c >= '0' && c <= '9' )
        return c - '0';
    if ( c >= 'a' && c <= 'f' )
        return c - 'a' + 10;
    if ( c >= 'A' && c <= 'F' )
        return c - 'A' + 10;
    return 0;
}


/* Decode string %xx -> char (in place) */
void urldecode(char *buf) {
  int v;
  char *p, *s, *w;

  w=p=buf;
  while (*p) {
    v=0;

    if (*p=='%') {
      s=p;
      s++;

      if (isxdigit((int) s[0]) && isxdigit((int) s[1]) ) {
        v=hexit(s[0])*16+hexit(s[1]);
        if (v) { /* do not decode %00 to null char */
          *w=(char)v;
          p=&s[1];
        }
      }

    }

    if (!v) *w=*p;
    p++; w++;
  }
  *w='\0';

  return;

}

#endif  /* ifdef CGI */

/* addnoise:
	adds noise with bandwidth, center frequency and SNR as defined in CWP
	noise amplitude is constant, the CW amplitude is scaled.
*/

void addnoise (int length, CWP *cw) {
	scalebuffer(cw->inpcm, cw->inpcm_size, snramplitude(cw->snr));
	addbuffer(cw->inpcm, cw->noisebuf, cw->inpcm_size);
	filterloop(cw->inpcm, cw->inpcm_size, cw->bandpassbw); 
}

/* snramplitude - returns the amplitude needed for a SNR of "snr" dB as
   float; noise average amplitude considered to be 0.25
   Currently implemented by a lookup table with fixed, precalculated values.
*/
float snramplitude (int snr) {
	/* Lookup table; first value = -10dB, last +10dB */
	float snrlookup[21] = { 0.042, 0.0475, 0.0533, 0.0599, 0.067, 0.075, 0.084, 0.094,
		0.1055, 0.1187, 0.134, 0.15, 0.168, 0.19, 0.213, 0.238, 0.267, 0.3, 0.335, 0.378, 0.425 };

	return snrlookup[snr+10];
}


void fillnoisebuffer (short int *buf, int size, float amplitude) {
	int i;
	
	for (i=0; i < size; i++) {
		buf[i] = (short int) (2.0*amplitude*(rand()/(1.0*RAND_MAX)-0.5));
	}
}


/*
 * Digital filter designed by mkfilter/mkshape/gencode A.J. Fisher Command 
 * line: /www/usr/fisher/helpers/mkfilter -Bu -Bp -o 3 -a 3.7500000000e-02 
 * 1.6250000000e-01 -l 
 *
 * Modified to filter "in place" with one array (buf) of length l
 */

void filterloop (short int *buf, int l, int b) {
  static float xv[7], yv[7];
  short int *in;

  /* 4 filters with each 6 coefficients */
  static float c[4][6] = {
    /* 100Hz */
    {-0.6235385946, 3.3779247772, -8.2824221345, 11.5675604240,
     -9.6967045468, 4.6299593645},
    /* 500Hz */
    {-0.4535459334, 2.5370880100, -6.4847845669, 9.5144072211,
     -8.4472239809, 4.3051177389},
    /* 1000Hz */
    {-0.1978251873, 1.3168771088, -3.8803884946, 6.5449240143,
     -6.6950970397, 3.9046544087},
    /* 2100Hz */
    {0.0131505881, 0.2450198734, -0.6698556894, 0.9667482217,
     -1.8996423954, 2.3375093931}
  };

  static float gain[4] = { 7.637953014e+02, 1.886640723e+02, 3.154970680e+01,
    5.346000407e+00 };

  long int k;

  /* bandwidth -> filter parameters */
  if      (b < 500 ) { b = 0; }
  else if (b < 1000) { b = 1; }
  else if (b < 2100) { b = 2; }
  else               { b = 4; }

  in = calloc(l, sizeof (short int));
  memcpy(in, buf, l*sizeof(short int));

  for (k = 0; k < l; k++)
    {
      xv[0] = xv[1]; xv[1] = xv[2]; xv[2] = xv[3]; 
	  xv[3] = xv[4]; xv[4] = xv[5]; xv[5] = xv[6];

	  xv[6] = ((float) in[k] / 255) / gain[b];

	  yv[0] = yv[1]; yv[1] = yv[2]; yv[2] = yv[3];
	  yv[3] = yv[4]; yv[4] = yv[5]; yv[5] = yv[6];

      yv[6] = (xv[6] - xv[0]) + 3 * (xv[2] - xv[4])
		+ (c[b][0] * yv[0]) + (c[b][1] * yv[1])
		+ (c[b][2] * yv[2]) + (c[b][3] * yv[3])
		+ (c[b][4] * yv[4]) + (c[b][5] * yv[5]);
   
   	  buf[k] = (short int) (yv[6] * 255.0);
    }
}


void scalebuffer(short int *buf, int length, float factor) {
	int i;
	for (i=0; i < length; i++) {
		buf[i] = (short int) buf[i]*factor;
	}
}


/* Adds buffer b2 to b1 with length l */

void addbuffer (short int *b1, short int *b2, int l) {
	int i;
	for (i=0; i < l; i++) {
		b1[i] += b2[i];
	}
}



void init_encoder (CWP *cw) {
	ogg_packet       hdr;
	ogg_packet       hdr_comm;
	ogg_packet       hdr_code;

	if (cw->encoder == MP3) {
		gfp = lame_init();
		lame_set_num_channels(gfp,1);
		lame_set_in_samplerate(gfp, cw->samplerate);
		lame_set_brate(gfp, cw->brate);
		lame_set_mode(gfp,1);
		lame_set_quality(gfp, cw->quality); 
			
		if (lame_init_params(gfp) < 0) {
			fprintf(stderr, "Failed: lame_init_params(gfp) \n");
			exit(1);
		}
	}
	else {	/* OGG */
		vorbis_info_init(&vi);
		
		if (vorbis_encode_init_vbr(&vi,1,cw->samplerate,0.7)) {
			fprintf(stderr, "Failed: vorbis_encode_init_vbr()\n");
			exit(1);
		}

		vorbis_comment_init(&vc);
		vorbis_comment_add_tag(&vc,"ENCODER","ebook2cw");
		vorbis_analysis_init(&vd, &vi);
		vorbis_block_init(&vd, &vb);
		ogg_stream_init(&os,rand());

		vorbis_analysis_headerout(&vd,&vc,&hdr,&hdr_comm,&hdr_code);
		ogg_stream_packetin(&os,&hdr); 
		ogg_stream_packetin(&os,&hdr_comm);
		ogg_stream_packetin(&os,&hdr_code);

	}

}


void ogg_encode_and_write (CWP *cw) {
	/* vorbis_analysis_wrote() must have been called */

	/* Encode and write */
	while (vorbis_analysis_blockout(&vd,&vb) == 1) {
		vorbis_analysis(&vb, NULL);
		vorbis_bitrate_addblock(&vb);
			while (vorbis_bitrate_flushpacket(&vd,&op)) {
				ogg_stream_packetin(&os,&op);
				while (1) {
					int result = ogg_stream_pageout(&os,&og);
					if (result == 0) break;
					fwrite(og.header,1,og.header_len, cw->outfile);
					fwrite(og.body,1,og.body_len, cw->outfile);
/*					if(ogg_page_eos(&og)) eos = 1;*/
				}
			}
	}	
}


void encode_buffer (int length, CWP *cw) {
	int outbytes, i;
	float **buffer;			/* for OGG enc only */

	switch (cw->encoder) {
		case MP3:
			outbytes = lame_encode_buffer(gfp, cw->inpcm, cw->inpcm, length, 
					cw->mp3buffer, cw->mp3buffer_size);
			if (outbytes < 0) {
				fprintf(stderr, "Error: lame_encode_buffer returned %d. "
						"Exit.\n", outbytes);
				exit(EXIT_FAILURE);
			}

			if (fwrite(cw->mp3buffer, sizeof(char), outbytes, cw->outfile) 
					!= outbytes) {
				fprintf(stderr, "Error: Writing %db to file failed. Exit.\n", 
						outbytes);
				exit(EXIT_FAILURE);
			}
			break;
		case OGG:
			buffer = vorbis_analysis_buffer(&vd, length);
			for (i=0; i < length; i++) {
				buffer[0][i] = (float) cw->inpcm[i]/(1.8*CWAMPLITUDE);
			}
			vorbis_analysis_wrote(&vd,i);
			ogg_encode_and_write(cw);
			break;
	} /* switch */

}

char *timestring (int ms) {
	int h = 0, m = 0, s = 0;
	static char t[32];
	while (ms > 3600000) {			/* hours */
		h++;
		ms -= 3600000;
	}
	while (ms > 60000) {			/* minutes */
		m++;
		ms -= 60000;
	}
	while (ms > 1000) {				/* seconds */
		s++;
		ms -= 1000;
	}

	if (h) {
		snprintf(t, 31, "%d:%02d:%02d", h, m, s);
	}
	else if (m) {
		snprintf(t, 31, "%02d:%02d", m, s);
	}
	else if (s) {
		snprintf(t, 31, "%ds", s);
	}

	return t;
}


/* tries to guess the encoding (UTF8, ISO8859-1) of a file 
 * 
 * Indicator for UTF-8 (2-byte chars): 110xxxxx 10xxxxxx
 *
 */

void guessencoding (char *filename) {
	FILE *infile;
	unsigned int c;
	int is_iso   = 1;

	if ((infile = fopen(filename, "r")) == NULL) {
		fprintf(stderr, "Error: Cannot open file %s. Exit.\n", filename);
		exit(EXIT_FAILURE);
	}

	printf("Guessed file format of %s: ", filename);

	while ((c = getc(infile)) != EOF) {
		if ((c & 224) == 192) {		/* check if highest 3 bits are 110=224 */
			c = getc(infile);		/* next byte 10xxxxxx? */
			if ((c & 192) == 128) {
				is_iso = 0;
				break;
			}
		}
	}

	printf("%s\n\n", (is_iso ? "ISO 8859-1" : "UTF-8")); 

	exit(EXIT_SUCCESS);
}


/* vim: noai:ts=4:sw=4 
 * */
