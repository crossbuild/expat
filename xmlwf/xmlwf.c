/*
The contents of this file are subject to the Mozilla Public License
Version 1.0 (the "License"); you may not use this file except in
compliance with the License. You may obtain a copy of the License at
http://www.mozilla.org/MPL/

Software distributed under the License is distributed on an "AS IS"
basis, WITHOUT WARRANTY OF ANY KIND, either express or implied. See the
License for the specific language governing rights and limitations
under the License.

The Original Code is expat.

The Initial Developer of the Original Code is James Clark.
Portions created by James Clark are Copyright (C) 1998
James Clark. All Rights Reserved.

Contributor(s):
*/

#include "xmlparse.h"
#include "filemap.h"

#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <fcntl.h>

#ifdef _MSC_VER
#include <io.h>
#endif

#ifdef _POSIX_SOURCE
#include <unistd.h>
#endif

#ifndef O_BINARY
#ifdef _O_BINARY
#define O_BINARY _O_BINARY
#else
#define O_BINARY 0
#endif
#endif

#ifdef _MSC_VER
#include <crtdbg.h>
#endif

#ifdef _DEBUG
#define READ_SIZE 16
#else
#define READ_SIZE (1024*8)
#endif

#ifdef XML_UNICODE
#define T(x) L ## x
#define ftprintf fwprintf
#define tfopen _wfopen
#define fputts fputws
#define puttc putwc
#define tcscmp wcscmp
#define tcscpy wcscpy
#define tcscat wcscat
#define tcschr wcschr
#define tcsrchr wcsrchr
#define tcslen wcslen
#define tperror _wperror
#define topen _wopen
#define tmain wmain
#define tremove _wremove
#else /* not XML_UNICODE */
#define T(x) x
#define ftprintf fprintf
#define tfopen fopen
#define fputts fputs
#define puttc putc
#define tcscmp strcmp
#define tcscpy strcpy
#define tcscat strcat
#define tcschr strchr
#define tcsrchr strrchr
#define tcslen strlen
#define tperror perror
#define topen open
#define tmain main
#define tremove remove
#endif /* not XML_UNICODE */

static void characterData(void *userData, const XML_Char *s, int len)
{
  FILE *fp = userData;
  for (; len > 0; --len, ++s) {
    switch (*s) {
    case T('&'):
      fputts(T("&amp;"), fp);
      break;
    case T('<'):
      fputts(T("&lt;"), fp);
      break;
    case T('>'):
      fputts(T("&gt;"), fp);
      break;
    case T('"'):
      fputts(T("&quot;"), fp);
      break;
    case 9:
    case 10:
    case 13:
      ftprintf(fp, T("&#%d;"), *s);
      break;
    default:
      puttc(*s, fp);
      break;
    }
  }
}

/* Lexicographically comparing UTF-8 encoded attribute values,
is equivalent to lexicographically comparing based on the character number. */

static int attcmp(const void *att1, const void *att2)
{
  return tcscmp(*(const XML_Char **)att1, *(const XML_Char **)att2);
}

static void startElement(void *userData, const XML_Char *name, const XML_Char **atts)
{
  int nAtts;
  const XML_Char **p;
  FILE *fp = userData;
  puttc(T('<'), fp);
  fputts(name, fp);

  p = atts;
  while (*p)
    ++p;
  nAtts = (p - atts) >> 1;
  if (nAtts > 1)
    qsort((void *)atts, nAtts, sizeof(XML_Char *) * 2, attcmp);
  while (*atts) {
    puttc(T(' '), fp);
    fputts(*atts++, fp);
    puttc(T('='), fp);
    puttc(T('"'), fp);
    characterData(userData, *atts, tcslen(*atts));
    puttc(T('"'), fp);
    atts++;
  }
  puttc(T('>'), fp);
}

static void endElement(void *userData, const XML_Char *name)
{
  FILE *fp = userData;
  puttc(T('<'), fp);
  puttc(T('/'), fp);
  fputts(name, fp);
  puttc(T('>'), fp);
}

static void processingInstruction(void *userData, const XML_Char *target, const XML_Char *data)
{
  FILE *fp = userData;
  puttc(T('<'), fp);
  puttc(T('?'), fp);
  fputts(target, fp);
  puttc(T(' '), fp);
  fputts(data, fp);
  puttc(T('?'), fp);
  puttc(T('>'), fp);
}

typedef struct {
  XML_Parser parser;
  int *retPtr;
} PROCESS_ARGS;

static
void reportError(XML_Parser parser, const XML_Char *filename)
{
  int code = XML_GetErrorCode(parser);
  const XML_Char *message = XML_ErrorString(code);
  if (message)
    ftprintf(stdout, T("%s:%d:%ld: %s\n"),
	     filename,
	     XML_GetErrorLineNumber(parser),
	     XML_GetErrorColumnNumber(parser),
	     message);
  else
    ftprintf(stderr, T("%s: (unknown message %d)\n"), filename, code);
}

static
void processFile(const void *data, size_t size, const XML_Char *filename, void *args)
{
  XML_Parser parser = ((PROCESS_ARGS *)args)->parser;
  int *retPtr = ((PROCESS_ARGS *)args)->retPtr;
  if (!XML_Parse(parser, data, size, 1)) {
    reportError(parser, filename);
    *retPtr = 0;
  }
  else
    *retPtr = 1;
}

static
int isAsciiLetter(XML_Char c)
{
  return (T('a') <= c && c <= T('z')) || (T('A') <= c && c <= T('Z'));
}

static
const XML_Char *resolveSystemId(const XML_Char *base, const XML_Char *systemId, XML_Char **toFree)
{
  XML_Char *s;
  *toFree = 0;
  if (!base
      || *systemId == T('/')
#ifdef WIN32
      || *systemId == T('\\')
      || (isAsciiLetter(systemId[0]) && systemId[1] == T(':'))
#endif
     )
    return systemId;
  *toFree = (XML_Char *)malloc((tcslen(base) + tcslen(systemId) + 2)*sizeof(XML_Char));
  if (!*toFree)
    return systemId;
  tcscpy(*toFree, base);
  s = *toFree;
  if (tcsrchr(s, T('/')))
    s = tcsrchr(s, T('/')) + 1;
#ifdef WIN32
  if (tcsrchr(s, T('\\')))
    s = tcsrchr(s, T('\\')) + 1;
#endif
  tcscpy(s, systemId);
  return *toFree;
}

static
int externalEntityRefFilemap(XML_Parser parser,
			     const XML_Char *openEntityNames,
			     const XML_Char *base,
			     const XML_Char *systemId,
			     const XML_Char *publicId)
{
  int result;
  XML_Char *s;
  XML_Parser entParser = XML_ExternalEntityParserCreate(parser, openEntityNames, 0);
  PROCESS_ARGS args;
  args.retPtr = &result;
  args.parser = entParser;
  if (!filemap(resolveSystemId(base, systemId, &s), processFile, &args))
    result = 0;
  free(s);
  XML_ParserFree(entParser);
  return result;
}

static
int processStream(const XML_Char *filename, XML_Parser parser)
{
  int fd = topen(filename, O_BINARY|O_RDONLY);
  if (fd < 0) {
    tperror(filename);
    return 0;
  }
  for (;;) {
    int nread;
    char *buf = XML_GetBuffer(parser, READ_SIZE);
    if (!buf) {
      close(fd);
      ftprintf(stderr, T("%s: out of memory\n"), filename);
      return 0;
    }
    nread = read(fd, buf, READ_SIZE);
    if (nread < 0) {
      tperror(filename);
      close(fd);
      return 0;
    }
    if (!XML_ParseBuffer(parser, nread, nread == 0)) {
      reportError(parser, filename);
      close(fd);
      return 0;
    }
    if (nread == 0) {
      close(fd);
      break;;
    }
  }
  return 1;
}

static
int externalEntityRefStream(XML_Parser parser,
			    const XML_Char *openEntityNames,
			    const XML_Char *base,
			    const XML_Char *systemId,
			    const XML_Char *publicId)
{
  XML_Char *s;
  XML_Parser entParser = XML_ExternalEntityParserCreate(parser, openEntityNames, 0);
  int ret = processStream(resolveSystemId(base, systemId, &s), entParser);
  free(s);
  XML_ParserFree(entParser);
  return ret;
}

static
void usage(const XML_Char *prog)
{
  ftprintf(stderr, T("usage: %s [-r] [-x] [-d output-dir] [-e encoding] file ...\n"), prog);
  exit(1);
}

int tmain(int argc, XML_Char **argv)
{
  int i;
  const XML_Char *outputDir = 0;
  const XML_Char *encoding = 0;
  int useFilemap = 1;
  int processExternalEntities = 0;

#ifdef _MSC_VER
  _CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF|_CRTDBG_LEAK_CHECK_DF);
#endif

  i = 1;
  while (i < argc && argv[i][0] == T('-')) {
    int j;
    if (argv[i][1] == T('-') && argv[i][2] == T('\0')) {
      i++;
      break;
    }
    j = 1;
    if (argv[i][j] == T('r')) {
      useFilemap = 0;
      j++;
    }
    if (argv[i][j] == T('x')) {
      processExternalEntities = 1;
      j++;
    }
    if (argv[i][j] == T('d')) {
      if (argv[i][j + 1] == T('\0')) {
	if (++i == argc)
	  usage(argv[0]);
	outputDir = argv[i];
      }
      else
	outputDir = argv[i] + j + 1;
      i++;
    }
    else if (argv[i][j] == T('e')) {
      if (argv[i][j + 1] == T('\0')) {
	if (++i == argc)
	  usage(argv[0]);
	encoding = argv[i];
      }
      else
	encoding = argv[i] + j + 1;
      i++;
    }
    else if (argv[i][j] == T('\0') && j > 1)
      i++;
    else
      usage(argv[0]);
  }
  if (i == argc)
    usage(argv[0]);
  for (; i < argc; i++) {
    FILE *fp = 0;
    XML_Char *outName = 0;
    int result;
    XML_Parser parser = XML_ParserCreate(encoding);
    if (outputDir) {
      const XML_Char *file = argv[i];
      if (tcsrchr(file, T('/')))
	file = tcsrchr(file, T('/')) + 1;
#ifdef WIN32
      if (tcsrchr(file, T('\\')))
	file = tcsrchr(file, T('\\')) + 1;
#endif
      outName = malloc((tcslen(outputDir) + tcslen(file) + 2) * sizeof(XML_Char));
      tcscpy(outName, outputDir);
      tcscat(outName, T("/"));
      tcscat(outName, file);
      fp = tfopen(outName, T("wb"));
      if (!fp) {
	tperror(outName);
	exit(1);
      }
#ifdef XML_UNICODE
      puttc(0xFEFF, fp);
#endif
      XML_SetUserData(parser, fp);
      XML_SetElementHandler(parser, startElement, endElement);
      XML_SetCharacterDataHandler(parser, characterData);
      XML_SetProcessingInstructionHandler(parser, processingInstruction);
    }
    if (processExternalEntities) {
      if (!XML_SetBase(parser, argv[i])) {
	ftprintf(stderr, T("%s: out of memory"), argv[0]);
	exit(1);
      }
      XML_SetExternalEntityRefHandler(parser,
	                              useFilemap
				      ? externalEntityRefFilemap
				      : externalEntityRefStream);
    }
    if (useFilemap) {
      PROCESS_ARGS args;
      args.retPtr = &result;
      args.parser = parser;
      if (!filemap(argv[i], processFile, &args))
	result = 0;
    }
    else
      result = processStream(argv[i], parser);
    if (outputDir) {
      fclose(fp);
      if (!result)
	tremove(outName);
      free(outName);
    }
    XML_ParserFree(parser);
  }
  return 0;
}
