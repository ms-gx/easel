/* esl_dsqdata : faster sequence input
 *
 * Implements a predigitized binary file format for biological
 * sequences. Sequence data are packed bitwise into 32-bit packets,
 * where each packet contains either six 5-bit residues or fifteen
 * 2-bit residues, plus two control bits.  Input is asynchronous,
 * using POSIX threads, with a "reader" thread doing disk reads and an
 * "unpacker" thread preparing chunks of sequences for
 * analysis. Sequence data and metadata are stored in separate files,
 * which sometimes may allow further input acceleration by deferring
 * metadata accesses until they're actually needed.
 * 
 * A DSQDATA database <basename> is stored in four files:
 *    - basename       : a human-readable stub
 *    - basename.dsqi  : index file, enabling random access and parallel chunking
 *    - basename.dsqm  : metadata including names, accessions, descriptions, taxonomy
 *    - basename.dsqs  : sequences, in a packed binary format
 * 
 * Contents:
 *   1. ESL_DSQDATA: reading dsqdata format
 *   2. Creating dsqdata format from a sequence file
 *   3. ESL_DSQDATA_CHUNK, a chunk of input sequence data
 *   4. Loader and unpacker, the input threads
 *   5. Packing sequences and unpacking chunks
 *   6. Notes and references
 *   7. Unit tests
 *   8. Test driver
 *   9. Examples
 */

#include "easel.h"
#include "esl_alphabet.h"
#include "esl_dsqdata.h"
#include "esl_random.h"
#include "esl_sq.h"
#include "esl_sqio.h"

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>

static ESL_DSQDATA_CHUNK *dsqdata_chunk_Create (ESL_DSQDATA *dd);
static void               dsqdata_chunk_Destroy(ESL_DSQDATA_CHUNK *chu);

static void *dsqdata_loader_thread  (void *p);
static void *dsqdata_unpacker_thread(void *p);

static int   dsqdata_unpack_chunk(ESL_DSQDATA_CHUNK *chu);
static int   dsqdata_pack5 (ESL_DSQ *dsq, int n, uint32_t **ret_psq, int *ret_plen);
static int   dsqdata_pack2 (ESL_DSQ *dsq, int n, uint32_t **ret_psq, int *ret_plen);

/*****************************************************************
 * 1. ESL_DSQDATA: reading dsqdata format
 *****************************************************************/

/* Function:  esl_dsqdata_Open()
 * Synopsis:  Open a digital sequence database for reading
 * Incept:    SRE, Wed Jan 20 09:50:00 2016 [Amtrak 2150, NYP-BOS]
 *
 * Purpose:   Open digital sequence database <basename> for reading.
 *            Configure it for a specified number of 1 or
 *            more parallelized <nconsumers>. The consumers are one or
 *            more threads that are processing chunks of data in
 *            parallel.
 *            
 *            The file <basename> is a human-readable stub describing
 *            the database. The bulk of the data are in three
 *            accompanying binary files: the index file
 *            <basename>.dsqi, the metadata file <basename>.dsqm, and
 *            the sequence file <basename>.dsqs.
 *            
 *            <byp_abc> provides a way to either tell <dsqdata> to
 *            expect a specific alphabet in the <basename> database
 *            (and return a normal failure on a mismatch), or, when
 *            the alphabet remains unknown, to figure out the alphabet
 *            in <basename> is and allocate and return a new alphabet.
 *            <byp_abc> uses a partial Easel "bypass" idiom for this:
 *            if <*byp_abc> is NULL, we allocate and return a new
 *            alphabet; if <*byp_abc> is a ptr to an existing
 *            alphabet, we use it for validation. That is,
 *                
 *            \begin{cchunk}
 *                abc = NULL;
 *                esl_dsqdata_Open(&abc, basename...)
 *                // <abc> is now the alphabet of <basename>; 
 *                // you're responsible for Destroy'ing it
 *            \end{cchunk}
 *                
 *            or:
 *            \begin{cchunk}
 *                abc = esl_alphabet_Create(eslAMINO);
 *                status = esl_dsqdata_Open(&abc, basename);
 *                // if status == eslEINCOMPAT, alphabet in basename 
 *                // doesn't match caller's expectation
 *            \end{cchunk}
 *
 * Args:      byp_abc    : optional alphabet hint; pass &abc or NULL.
 *            basename   : data are in files <basename> and <basename.dsq[ism]>
 *            nconsumers : number of consumer threads caller is going to Read() with
 *            ret_dd     : RETURN : the new ESL_DSQDATA object.
 *
 * Returns:   <eslOK> on success.
 * 
 *            <eslENOTFOUND> if one or more of the expected datafiles
 *            aren't there or can't be opened.
 *
 *            <eslEFORMAT> if something looks wrong in parsing file
 *            formats.  Includes problems in headers, and also the
 *            case where caller provides a digital alphabet in
 *            <*byp_abc> and it doesn't match the database's alphabet.
 *
 *            On any normal error, <*ret_dd> is still returned, but in
 *            an error state, and <dd->errbuf> is a user-directed
 *            error message that the caller can relay to the user. Other
 *            than the <errbuf>, the rest of the contents are undefined.
 *
 * Throws:    <eslEMEM> on allocation error.
 *            <eslESYS> on system call failure.
 * 
 *            On any thrown exception, <*ret_dd> is returned NULL.
 */
int
esl_dsqdata_Open(ESL_ALPHABET **byp_abc, char *basename, int nconsumers, ESL_DSQDATA **ret_dd)
{
  ESL_DSQDATA *dd        = NULL;
  int          bufsize   = 4096;
  uint32_t     magic     = 0;
  uint32_t     tag       = 0;
  uint32_t     alphatype = eslUNKNOWN;
  char        *p;                       // used for strtok() parsing of fields on a line
  char         buf[4096];
  int          status;
  
  ESL_DASSERT1(( nconsumers > 0 ));
  
  ESL_ALLOC(dd, sizeof(ESL_DSQDATA));
  dd->stubfp          = NULL;
  dd->ifp             = NULL;
  dd->sfp             = NULL;
  dd->mfp             = NULL;
  dd->abc_r           = *byp_abc;        // This may be NULL; if so, we create it later.
  dd->magic           = 0;
  dd->uniquetag       = 0;
  dd->flags           = 0;
  dd->max_namelen     = 0;
  dd->max_acclen      = 0;
  dd->max_desclen     = 0;
  dd->max_seqlen      = 0;
  dd->nseq            = 0;
  dd->nres            = 0;

  dd->chunk_maxseq    = eslDSQDATA_CHUNK_MAXSEQ;    // someday we may want to allow tuning these
  dd->chunk_maxpacket = eslDSQDATA_CHUNK_MAXPACKET;
  dd->do_byteswap     = FALSE;
  dd->pack5           = FALSE;  

  dd->nconsumers      = nconsumers;
  dd->loader_outbox   = NULL;
  dd->unpacker_outbox = NULL;
  dd->recycling       = NULL;
  dd->errbuf[0]       = '\0';
  dd->at_eof          = FALSE;
  dd->lt_c = dd->lom_c = dd->lof_c = dd->loe_c = FALSE;  
  dd->ut_c = dd->uom_c = dd->uof_c = dd->uoe_c = FALSE;
  dd->rm_c = dd->r_c   = FALSE;
  dd->errbuf[0] = '\0';

  /* Open the four files.
   */
  ESL_ALLOC( dd->basename, sizeof(char) * (strlen(basename) + 6)); // +5 for .dsqx; +1 for \0
  if ( sprintf(dd->basename, "%s.dsqi", basename) <= 0)   ESL_XEXCEPTION_SYS(eslESYS, "sprintf() failure");
  if (( dd->ifp = fopen(dd->basename, "rb"))   == NULL)   ESL_XFAIL(eslENOTFOUND, dd->errbuf, "Failed to find or open index file %s\n", dd->basename);

  if ( sprintf(dd->basename, "%s.dsqm", basename) <= 0)   ESL_XEXCEPTION_SYS(eslESYS, "sprintf() failure");
  if (( dd->mfp = fopen(dd->basename, "rb"))   == NULL)   ESL_XFAIL(eslENOTFOUND, dd->errbuf, "Failed to find or open metadata file %s\n", dd->basename);

  if ( sprintf(dd->basename, "%s.dsqs", basename) <= 0)   ESL_XEXCEPTION_SYS(eslESYS, "sprintf() failure");
  if (( dd->sfp = fopen(dd->basename, "rb"))   == NULL)   ESL_XFAIL(eslENOTFOUND, dd->errbuf, "Failed to find or open sequence file %s\n", dd->basename);

  strcpy(dd->basename, basename);
  if (( dd->stubfp = fopen(dd->basename, "r")) == NULL)   ESL_XFAIL(eslENOTFOUND, dd->errbuf, "Failed to find or open stub file %s\n", dd->basename);

  /* The stub file is unparsed, intended to be human readable, with one exception:
   * The first line contains the unique tag that we use to validate linkage of the 4 files.
   * The format of that first line is:
   *     Easel dsqdata v123 x0000000000 
   */
  if ( fgets(buf, bufsize, dd->stubfp) == NULL)           ESL_XFAIL(eslEFORMAT, dd->errbuf, "stub file is empty - no tag line found");
  if (( p = strtok(buf,  " \t\n\r"))   == NULL)           ESL_XFAIL(eslEFORMAT, dd->errbuf, "stub file has bad format: tag line has no data");
  if (  strcmp(p, "Easel") != 0)                          ESL_XFAIL(eslEFORMAT, dd->errbuf, "stub file has bad format in tag line");
  if (( p = strtok(NULL, " \t\n\r"))   == NULL)           ESL_XFAIL(eslEFORMAT, dd->errbuf, "stub file has bad format in tag line");
  if (  strcmp(p, "dsqdata") != 0)                        ESL_XFAIL(eslEFORMAT, dd->errbuf, "stub file has bad format in tag line");
  if (( p = strtok(NULL, " \t\n\r"))   == NULL)           ESL_XFAIL(eslEFORMAT, dd->errbuf, "stub file has bad format in tag line");
  if ( *p != 'v')                                         ESL_XFAIL(eslEFORMAT, dd->errbuf, "stub file has bad format: no v on version");                        
  if ( ! esl_str_IsInteger(p+1))                          ESL_XFAIL(eslEFORMAT, dd->errbuf, "stub file had bad format: no version number");
  // version number is currently unused: there's only 1
  if (( p = strtok(NULL, " \t\n\r"))   == NULL)           ESL_XFAIL(eslEFORMAT, dd->errbuf, "stub file has bad format in tag line");
  if ( *p != 'x')                                         ESL_XFAIL(eslEFORMAT, dd->errbuf, "stub file has bad format: no x on tag");                        
  if ( ! esl_str_IsInteger(p+1))                          ESL_XFAIL(eslEFORMAT, dd->errbuf, "stub file had bad format: no integer tag");
  dd->uniquetag = strtoul(p+1, NULL, 10);
    
  /* Index file has a header of 7 uint32's, 3 uint64's */
  if ( fread(&(dd->magic),       sizeof(uint32_t), 1, dd->ifp) != 1) ESL_XFAIL(eslEFORMAT, dd->errbuf, "index file has no header - is empty?");
  if ( fread(&tag,               sizeof(uint32_t), 1, dd->ifp) != 1) ESL_XFAIL(eslEFORMAT, dd->errbuf, "index file header truncated, no tag");
  if ( fread(&alphatype,         sizeof(uint32_t), 1, dd->ifp) != 1) ESL_XFAIL(eslEFORMAT, dd->errbuf, "index file header truncated, no alphatype");
  if ( fread(&(dd->flags),       sizeof(uint32_t), 1, dd->ifp) != 1) ESL_XFAIL(eslEFORMAT, dd->errbuf, "index file header truncated, no flags");
  if ( fread(&(dd->max_namelen), sizeof(uint32_t), 1, dd->ifp) != 1) ESL_XFAIL(eslEFORMAT, dd->errbuf, "index file header truncated, no max name len");
  if ( fread(&(dd->max_acclen),  sizeof(uint32_t), 1, dd->ifp) != 1) ESL_XFAIL(eslEFORMAT, dd->errbuf, "index file header truncated, no max accession len");
  if ( fread(&(dd->max_desclen), sizeof(uint32_t), 1, dd->ifp) != 1) ESL_XFAIL(eslEFORMAT, dd->errbuf, "index file header truncated, no max description len");

  if ( fread(&(dd->max_seqlen),  sizeof(uint64_t), 1, dd->ifp) != 1) ESL_XFAIL(eslEFORMAT, dd->errbuf, "index file header truncated, no max seq len");
  if ( fread(&(dd->nseq),        sizeof(uint64_t), 1, dd->ifp) != 1) ESL_XFAIL(eslEFORMAT, dd->errbuf, "index file header truncated, no nseq");
  if ( fread(&(dd->nres),        sizeof(uint64_t), 1, dd->ifp) != 1) ESL_XFAIL(eslEFORMAT, dd->errbuf, "index file header truncated, no nres");

  /* Check the magic and the tag */
  if      (tag != dd->uniquetag)                 ESL_XFAIL(eslEFORMAT, dd->errbuf, "index file has bad tag, doesn't go with stub file");
  if      (dd->magic == eslDSQDATA_MAGIC_V1SWAP) dd->do_byteswap = TRUE;
  else if (dd->magic != eslDSQDATA_MAGIC_V1)     ESL_XFAIL(eslEFORMAT, dd->errbuf, "index file has bad magic");

  /* Either validate, or create the alphabet */
  if  (dd->abc_r)
    {
      if (alphatype != dd->abc_r->type) 
	ESL_XFAIL(eslEFORMAT, dd->errbuf, "data files use %s alphabet; expected %s alphabet", 
		  esl_abc_DecodeType(alphatype), 
		  esl_abc_DecodeType(dd->abc_r->type));
    }
  else
    {
      if ( esl_abc_ValidateType(alphatype)             != eslOK) ESL_XFAIL(eslEFORMAT, dd->errbuf, "index file has invalid alphabet type %d", alphatype);
      if (( dd->abc_r = esl_alphabet_Create(alphatype)) == NULL) ESL_XEXCEPTION(eslEMEM, "alphabet creation failed");
    }

  /* If it's protein, flip the switch to expect all 5-bit packing */
  if (dd->abc_r->type == eslAMINO) dd->pack5 = TRUE;

  /* Metadata file has a header of 2 uint32's, magic and uniquetag */
  if (( fread(&magic, sizeof(uint32_t), 1, dd->mfp)) != 1) ESL_XFAIL(eslEFORMAT, dd->errbuf, "metadata file has no header - is empty?");
  if (( fread(&tag,   sizeof(uint32_t), 1, dd->mfp)) != 1) ESL_XFAIL(eslEFORMAT, dd->errbuf, "metadata file header truncated - no tag?");
  if ( magic != dd->magic)                                 ESL_XFAIL(eslEFORMAT, dd->errbuf, "metadata file has bad magic");
  if ( tag   != dd->uniquetag)                             ESL_XFAIL(eslEFORMAT, dd->errbuf, "metadata file has bad tag, doesn't match stub");

  /* Sequence file also has a header of 2 uint32's, magic and uniquetag */
  if (( fread(&magic, sizeof(uint32_t), 1, dd->sfp)) != 1) ESL_XFAIL(eslEFORMAT, dd->errbuf, "sequence file has no header - is empty?");
  if (( fread(&tag,   sizeof(uint32_t), 1, dd->sfp)) != 1) ESL_XFAIL(eslEFORMAT, dd->errbuf, "sequence file header truncated - no tag?");
  if ( magic != dd->magic)                                 ESL_XFAIL(eslEFORMAT, dd->errbuf, "sequence file has bad magic");
  if ( tag   != dd->uniquetag)                             ESL_XFAIL(eslEFORMAT, dd->errbuf, "sequence file has bad tag, doesn't match stub");

  /* Create the loader and unpacker threads.
   */
  if ( pthread_mutex_init(&dd->loader_outbox_mutex,      NULL) != 0) ESL_XEXCEPTION(eslESYS, "pthread_mutex_init() failed");    dd->lom_c = TRUE;
  if ( pthread_mutex_init(&dd->unpacker_outbox_mutex,    NULL) != 0) ESL_XEXCEPTION(eslESYS, "pthread_mutex_init() failed");    dd->uom_c = TRUE;
  if ( pthread_mutex_init(&dd->recycling_mutex,          NULL) != 0) ESL_XEXCEPTION(eslESYS, "pthread_mutex_init() failed");    dd->rm_c  = TRUE;

  if ( pthread_cond_init(&dd->loader_outbox_full_cv,     NULL) != 0) ESL_XEXCEPTION(eslESYS, "pthread_cond_init() failed");     dd->lof_c = TRUE;
  if ( pthread_cond_init(&dd->loader_outbox_empty_cv,    NULL) != 0) ESL_XEXCEPTION(eslESYS, "pthread_cond_init() failed");     dd->loe_c = TRUE;
  if ( pthread_cond_init(&dd->unpacker_outbox_full_cv,   NULL) != 0) ESL_XEXCEPTION(eslESYS, "pthread_cond_init() failed");     dd->uof_c = TRUE;
  if ( pthread_cond_init(&dd->unpacker_outbox_empty_cv,  NULL) != 0) ESL_XEXCEPTION(eslESYS, "pthread_cond_init() failed");     dd->uoe_c = TRUE;
  if ( pthread_cond_init(&dd->recycling_cv,              NULL) != 0) ESL_XEXCEPTION(eslESYS, "pthread_cond_init() failed");     dd->r_c   = TRUE;
  
  if ( pthread_create(&dd->unpacker_t, NULL, dsqdata_unpacker_thread, dd) != 0) ESL_XEXCEPTION(eslESYS, "pthread_create() failed");  dd->ut_c = TRUE;
  if ( pthread_create(&dd->loader_t,   NULL, dsqdata_loader_thread,   dd) != 0) ESL_XEXCEPTION(eslESYS, "pthread_create() failed");  dd->lt_c = TRUE;

  *ret_dd  = dd;
  *byp_abc = dd->abc_r;     // If caller provided <*byp_abc> this is a no-op, because we set abc_r = *byp_abc.
  return eslOK;             //  .. otherwise we're passing the created <abc> back to caller, caller's
                            //     responsibility, we just keep the reference to it.
 ERROR:
  if (status == eslENOTFOUND || status == eslEFORMAT || status == eslEINCOMPAT)
    {    /* on normal errors, we return <dd> with its <errbuf>, don't change *byp_abc */
      *ret_dd  = dd;
      if (*byp_abc == NULL && dd->abc_r) esl_alphabet_Destroy(dd->abc_r);
      return status;
    }
  else
    {   /* on exceptions, we free <dd>, return it NULL, don't change *byp_abc */
      esl_dsqdata_Close(dd);
      *ret_dd = NULL;
      if (*byp_abc == NULL && dd->abc_r) esl_alphabet_Destroy(dd->abc_r);
      return status;
    }
}


/* Function:  esl_dsqdata_Read()
 * Synopsis:  Read next chunk of sequence data.
 * Incept:    SRE, Thu Jan 21 11:21:38 2016 [Harvard]
 *
 * Purpose:   Read the next chunk from <dd>, return a pointer to it in
 *            <*ret_chu>, and return <eslOK>. When data are exhausted,
 *            return <eslEOF>, and <*ret_chu> is <NULL>. 
 *
 *            Threadsafe. All thread operations in the dsqdata reader
 *            are handled internally. Caller does not have to worry
 *            about wrapping this in a mutex. Multiple caller threads
 *            can call <esl_dsqdata_Read()>.
 *
 *            All chunk allocation and deallocation is handled
 *            internally. After using a chunk, caller gives it back to
 *            the reader using <esl_dsqdata_Recycle()>.
 *
 * Args:      dd      : open dsqdata object to read from
 *            ret_chu : RETURN : next chunk of seq data
 *
 * Returns:   <eslOK> on success. <*ret_chu> is a chunk of seq data.
 *            Caller must call <esl_dsqdata_Recycle()> on each chunk
 *            that it Read()'s.
 *             
 *            <eslEOF> if we've reached the end of the input file;
 *            <*ret_chu> is NULL.
 *
 * Throws:    <eslESYS> if a pthread call fails. 
 *            Caller should treat this as disastrous. Without correctly
 *            working pthread calls, we cannot read, and we may not be able
 *            to correctly clean up and close the reader. Caller should
 *            treat <dd> as toxic, clean up whatever else it may need to,
 *            and exit.
 */
int
esl_dsqdata_Read(ESL_DSQDATA *dd, ESL_DSQDATA_CHUNK **ret_chu)
{
  ESL_DSQDATA_CHUNK *chu = NULL;

  /* The loader and unpacker have already done the work.  All that
   * _Read() needs to do is take a finished chunk from the unpacker's
   * outbox.  That finished chunk could be a final empty chunk, which
   * is the EOF signal.
   */
  
  /* If one reader has already processed eof, all subsequent Read() calls also return eslEOF */
  if (dd->at_eof) { *ret_chu = NULL; return eslEOF; }

  /* Get next chunk from unpacker. Wait if needed. */
  if ( pthread_mutex_lock(&dd->unpacker_outbox_mutex) != 0) ESL_EXCEPTION(eslESYS, "pthread call failed");
  while (dd->unpacker_outbox == NULL)
    {
      if ( pthread_cond_wait(&dd->unpacker_outbox_full_cv, &dd->unpacker_outbox_mutex) != 0) 
	ESL_EXCEPTION(eslESYS, "pthread call failed");
    }
  chu = dd->unpacker_outbox;
  dd->unpacker_outbox = NULL;
  if (! chu->N) dd->at_eof = TRUE;        // The eof flag makes sure only one reader processes EOF chunk.
  if ( pthread_mutex_unlock(&dd->unpacker_outbox_mutex)    != 0) ESL_EXCEPTION(eslESYS, "pthread call failed");
  if ( pthread_cond_signal (&dd->unpacker_outbox_empty_cv) != 0) ESL_EXCEPTION(eslESYS, "pthread call failed");
  
  /* If chunk has any data in it, go ahead and return it. */
  if (chu->N)
    {
      *ret_chu = chu;
      return eslOK;
    }
  /* Otherwise, an empty chunk is a signal that the loader and unpacker
   * are done. But the loader is responsible for freeing all the chunks
   * it allocated, so we have to get this chunk back to the loader, via
   * the recycling. (Alternatively, we could let the caller recycle 
   * the chunk on EOF, but letting the caller detect EOF on read and 
   * exit its loop, only recycling chunks inside the loop, is consistent
   * with all the rest of Easel's read idioms.
   */
  else
    {
      esl_dsqdata_Recycle(dd, chu);
      *ret_chu = NULL;
      return eslEOF;
    }
}


/* Function:  esl_dsqdata_Recycle()
 * Synopsis:  Give a chunk back to the reader.
 * Incept:    SRE, Thu Feb 11 19:24:33 2016
 *
 * Purpose:   Recycle chunk <chu> back to the reader <dd>.  The reader
 *            is responsible for all allocation and deallocation of
 *            chunks. The reader will either reuse the chunk's memory
 *            if more chunks remain to be read, or it will free it.
 *
 * Args:      dd  : the dsqdata reader
 *            chu : chunk to recycle
 *
 * Returns:   <eslOK> on success. 
 *
 * Throws:    <eslESYS> on a pthread call failure. Caller should regard
 *            such an error as disastrous; if pthread calls are
 *            failing, you cannot depend on the reader to be working
 *            at all, and you should treat <dd> as toxic. Do whatever
 *            desperate things you need to do and exit.
 */
int
esl_dsqdata_Recycle(ESL_DSQDATA *dd, ESL_DSQDATA_CHUNK *chu)
{
  if ( pthread_mutex_lock(&dd->recycling_mutex)   != 0) ESL_EXCEPTION(eslESYS, "pthread mutex lock failed");
  chu->nxt = dd->recycling;      // Push chunk onto head of recycling stack
  dd->recycling = chu;
  if ( pthread_mutex_unlock(&dd->recycling_mutex) != 0) ESL_EXCEPTION(eslESYS, "pthread mutex unlock failed");
  if ( pthread_cond_signal(&dd->recycling_cv)     != 0) ESL_EXCEPTION(eslESYS, "pthread cond signal failed");
  // That signal told the loader that there's a chunk it can recycle.
  return eslOK;
}



/* Function:  esl_dsqdata_Close()
 * Synopsis:  Close a dsqdata reader.
 * Incept:    SRE, Thu Feb 11 19:32:54 2016
 *
 * Purpose:   Close a dsqdata reader.
 *
 * Returns:   <eslOK> on success.

 * Throws:    <eslESYS> on a system call failure, including pthread
 *            calls and fclose(). Caller should regard such a failure
 *            as disastrous: treat <dd> as toxic and exit as soon as 
 *            possible without making any other system calls, if possible.
 */
int
esl_dsqdata_Close(ESL_DSQDATA *dd)
{
  if (dd)
    {
      if (dd->lt_c)   { if ( pthread_join(dd->loader_t,   NULL)                  != 0)  ESL_EXCEPTION(eslESYS, "pthread join failed");          }
      if (dd->ut_c)   { if ( pthread_join(dd->unpacker_t, NULL)                  != 0)  ESL_EXCEPTION(eslESYS, "pthread join failed");          }
      if (dd->lof_c)  { if ( pthread_cond_destroy(&dd->loader_outbox_full_cv)    != 0)  ESL_EXCEPTION(eslESYS, "pthread cond destroy failed");  }
      if (dd->loe_c)  { if ( pthread_cond_destroy(&dd->loader_outbox_empty_cv)   != 0)  ESL_EXCEPTION(eslESYS, "pthread cond destroy failed");  }
      if (dd->uof_c)  { if ( pthread_cond_destroy(&dd->unpacker_outbox_full_cv)  != 0)  ESL_EXCEPTION(eslESYS, "pthread cond destroy failed");  }
      if (dd->uoe_c)  { if ( pthread_cond_destroy(&dd->unpacker_outbox_empty_cv) != 0)  ESL_EXCEPTION(eslESYS, "pthread cond destroy failed");  }
      if (dd->r_c)    { if ( pthread_cond_destroy(&dd->recycling_cv)             != 0)  ESL_EXCEPTION(eslESYS, "pthread cond destroy failed");  }
      if (dd->lom_c)  { if ( pthread_mutex_destroy(&dd->loader_outbox_mutex)     != 0)  ESL_EXCEPTION(eslESYS, "pthread mutex destroy failed"); }
      if (dd->uom_c)  { if ( pthread_mutex_destroy(&dd->unpacker_outbox_mutex)   != 0)  ESL_EXCEPTION(eslESYS, "pthread mutex destroy failed"); }
      if (dd->rm_c)   { if ( pthread_mutex_destroy(&dd->recycling_mutex)         != 0)  ESL_EXCEPTION(eslESYS, "pthread mutex destroy failed"); }

      if (dd->ifp)    { if ( fclose(dd->ifp)    != 0) ESL_EXCEPTION(eslESYS, "fclose failed"); }
      if (dd->sfp)    { if ( fclose(dd->sfp)    != 0) ESL_EXCEPTION(eslESYS, "fclose failed"); }
      if (dd->mfp)    { if ( fclose(dd->mfp)    != 0) ESL_EXCEPTION(eslESYS, "fclose failed"); }
      if (dd->stubfp) { if ( fclose(dd->stubfp) != 0) ESL_EXCEPTION(eslESYS, "fclose failed"); }

      if (dd->basename) free(dd->basename);

      /* Loader thread is responsible for freeing all chunks it created, even on error. */
      ESL_DASSERT1(( dd->loader_outbox   == NULL ));
      ESL_DASSERT1(( dd->unpacker_outbox == NULL ));
      ESL_DASSERT1(( dd->recycling       == NULL ));

      free(dd);
    }
  return eslOK;
}


/*****************************************************************
 * 2. Creating dsqdata format from a sequence file
 *****************************************************************/

/* Function:  esl_dsqdata_Write()
 * Synopsis:  Create a dsqdata database
 * Incept:    SRE, Sat Feb 13 07:33:30 2016 [AGBT 2016, Orlando]
 *
 * Purpose:   Caller has just opened <sqfp>, in digital mode.
 *            Create a dsqdata database <basename> from the sequence
 *            data in <sqfp>.
 *
 *            <sqfp> must be protein, DNA, or RNA sequence data.  It
 *            must be rewindable (i.e. a file), because we have to
 *            read it twice. It must be newly opened (i.e. positioned
 *            at the start).
 *
 * Args:      sqfp     - newly opened sequence data file
 *            basename - base name of dsqdata files to create
 *            errbuf   - user-directed error message on normal errors
 *
 * Returns:   <eslOK> on success.
 *           
 *            <eslEWRITE> if an output file can't be opened. <errbuf>
 *            contains user-directed error message.
 *
 *            <eslEFORMAT> if a parse error is encountered while
 *            reading <sqfp>.
 * 
 *
 * Throws:    <eslESYS>   A system call failed, such as fwrite().
 *            <eslEINVAL> Sequence handle <sqfp> isn't digital and rewindable.
 *            <eslEMEM>   Allocation failure
 */
int
esl_dsqdata_Write(ESL_SQFILE *sqfp, char *basename, char *errbuf)
{
  ESL_RANDOMNESS *rng         = NULL;
  ESL_SQ         *sq          = NULL;
  FILE           *stubfp      = NULL;
  FILE           *ifp         = NULL;
  FILE           *mfp         = NULL;
  FILE           *sfp         = NULL;
  char           *outfile     = NULL;
  uint32_t        magic       = eslDSQDATA_MAGIC_V1;
  uint32_t        uniquetag;
  uint32_t        alphatype;
  uint32_t        flags       = 0;
  uint32_t        max_namelen = 0;
  uint32_t        max_acclen  = 0;
  uint32_t        max_desclen = 0;
  uint64_t        max_seqlen  = 0;
  uint64_t        nseq        = 0;
  uint64_t        nres        = 0;
  int             do_pack5    = FALSE;
  uint32_t       *psq;
  ESL_DSQDATA_RECORD idx;                    // one index record to write
  int             plen;
  int64_t         spos        = 0;
  int64_t         mpos        = 0;
  int             n;
  int             status;

  if (! esl_sqfile_IsRewindable(sqfp))  ESL_EXCEPTION(eslEINVAL, "sqfp must be rewindable (e.g. an open file)");
  if (! sqfp->abc)                      ESL_EXCEPTION(eslEINVAL, "sqfp must be digital");
  // Could also check that it's positioned at the start.
  if ( (sq = esl_sq_CreateDigital(sqfp->abc)) == NULL) { status = eslEMEM; goto ERROR; }


  /* First pass over the sequence file, to get statistics.
   * Read it now, before opening any files, in case we find any parse errors.
   */
  while ((status = esl_sqio_Read(sqfp, sq)) == eslOK)
    {
      nseq++;
      nres += sq->n;
      if (sq->n > max_seqlen) max_seqlen = sq->n;
      n = strlen(sq->name); if (n > max_namelen) max_namelen = n;
      n = strlen(sq->acc);  if (n > max_acclen)  max_acclen  = n;
      n = strlen(sq->desc); if (n > max_desclen) max_desclen = n;
      esl_sq_Reuse(sq);
    }
  if      (status == eslEFORMAT) ESL_XFAIL(eslEFORMAT, errbuf, sqfp->get_error(sqfp));
  else if (status != eslEOF)     return status;

  if ((status = esl_sqfile_Position(sqfp, 0)) != eslOK) return status;


  if ((    rng = esl_randomness_Create(0) )        == NULL)  { status = eslEMEM; goto ERROR; }
  uniquetag = esl_random_uint32(rng);
  alphatype = sqfp->abc->type;

  if      (alphatype == eslAMINO)                      do_pack5 = TRUE;
  else if (alphatype != eslDNA && alphatype != eslRNA) ESL_EXCEPTION(eslEINVAL, "alphabet must be protein or nucleic");


  if (( status = esl_sprintf(&outfile, "%s.dsqi", basename)) != eslOK) goto ERROR;
  if ((    ifp = fopen(outfile, "wb"))             == NULL)  ESL_XFAIL(eslEWRITE, errbuf, "failed to open dsqdata index file %s for writing", outfile);
  sprintf(outfile, "%s.dsqm", basename);
  if ((    mfp = fopen(outfile, "wb"))             == NULL)  ESL_XFAIL(eslEWRITE, errbuf, "failed to open dsqdata metadata file %s for writing", outfile);
  sprintf(outfile, "%s.dsqs", basename);
  if ((    sfp = fopen(outfile, "wb"))             == NULL)  ESL_XFAIL(eslEWRITE, errbuf, "failed to open dsqdata sequence file %s for writing", outfile);
  if (( stubfp = fopen(basename, "w"))             == NULL)  ESL_XFAIL(eslEWRITE, errbuf, "failed to open dsqdata stub file %s for writing", basename);


  

  /* Header: index file */
  if (fwrite(&magic,       sizeof(uint32_t), 1, ifp) != 1 ||
      fwrite(&uniquetag,   sizeof(uint32_t), 1, ifp) != 1 ||
      fwrite(&alphatype,   sizeof(uint32_t), 1, ifp) != 1 ||
      fwrite(&flags,       sizeof(uint32_t), 1, ifp) != 1 ||
      fwrite(&max_namelen, sizeof(uint32_t), 1, ifp) != 1 ||
      fwrite(&max_acclen,  sizeof(uint32_t), 1, ifp) != 1 ||
      fwrite(&max_desclen, sizeof(uint32_t), 1, ifp) != 1 ||
      fwrite(&max_seqlen,  sizeof(uint64_t), 1, ifp) != 1 ||
      fwrite(&nseq,        sizeof(uint64_t), 1, ifp) != 1 ||
      fwrite(&nres,        sizeof(uint64_t), 1, ifp) != 1) 
    ESL_XEXCEPTION_SYS(eslESYS, "fwrite() failed, index file header");

  /* Header: metadata file */
  if (fwrite(&magic,       sizeof(uint32_t), 1, mfp) != 1 ||
      fwrite(&uniquetag,   sizeof(uint32_t), 1, mfp) != 1)
    ESL_XEXCEPTION_SYS(eslESYS, "fwrite() failed, metadata file header");

  /* Header: sequence file */
  if (fwrite(&magic,       sizeof(uint32_t), 1, sfp) != 1 ||
      fwrite(&uniquetag,   sizeof(uint32_t), 1, sfp) != 1)
    ESL_XEXCEPTION_SYS(eslESYS, "fwrite() failed, metadata file header");

  /* Second pass: index, metadata, and sequence files */
  while ((status = esl_sqio_Read(sqfp, sq)) == eslOK)
    {
      /* Packed sequence */
      if (do_pack5) dsqdata_pack5(sq->dsq, sq->n, &psq, &plen);
      else          dsqdata_pack2(sq->dsq, sq->n, &psq, &plen);
      if ( fwrite(psq, sizeof(uint32_t), plen, sfp) != plen) 
	ESL_XEXCEPTION(eslESYS, "fwrite() failed, packed seq");
      spos += plen;

      /* Metadata */
      n = strlen(sq->name); 
      if ( fwrite(sq->name, sizeof(char), n+1, mfp) != n+1) 
	ESL_XEXCEPTION(eslESYS, "fwrite () failed, metadata, name");
      mpos += n+1;

      n = strlen(sq->acc);  
      if ( fwrite(sq->acc,  sizeof(char), n+1, mfp) != n+1) 
	ESL_XEXCEPTION(eslESYS, "fwrite () failed, metadata, accession");
      mpos += n+1;

      n = strlen(sq->desc); 
      if ( fwrite(sq->desc, sizeof(char), n+1, mfp) != n+1)
	ESL_XEXCEPTION(eslESYS, "fwrite () failed, metadata, description");
      mpos += n+1;

      if ( fwrite( &(sq->tax_id), sizeof(int32_t), 1, mfp) != 1)                  
	ESL_XEXCEPTION(eslESYS, "fwrite () failed, metadata, taxonomy id");
      mpos += sizeof(int32_t); 
      
      /* Index file */
      idx.psq_end      = spos-1;  // could be -1, on 1st seq, if 1st seq L=0.
      idx.metadata_end = mpos-1; 
      if ( fwrite(&idx, sizeof(ESL_DSQDATA_RECORD), 1, ifp) != 1) 
	ESL_XEXCEPTION(eslESYS, "fwrite () failed, index file");

      esl_sq_Reuse(sq);
    }

  /* Stub file */
  fprintf(stubfp, "Easel dsqdata v1 x%" PRIu32 "\n", uniquetag);
  fprintf(stubfp, "\n");
  fprintf(stubfp, "Original file:   %s\n",          sqfp->filename);
  fprintf(stubfp, "Original format: %s\n",          esl_sqio_DecodeFormat(sqfp->format));
  fprintf(stubfp, "Type:            %s\n",          esl_abc_DecodeType(sqfp->abc->type));
  fprintf(stubfp, "Sequences:       %" PRIu64 "\n", nseq);
  fprintf(stubfp, "Residues:        %" PRIu64 "\n", nres);
  
  esl_sq_Destroy(sq);
  esl_randomness_Destroy(rng);
  free(outfile);
  fclose(stubfp);
  fclose(ifp);
  fclose(mfp);
  fclose(sfp);
  return eslOK;

 ERROR:
  if (sq)      esl_sq_Destroy(sq);
  if (rng)     esl_randomness_Destroy(rng);
  if (outfile) free(outfile);
  if (stubfp)  fclose(stubfp);
  if (ifp)     fclose(ifp);
  if (mfp)     fclose(mfp);
  if (sfp)     fclose(sfp);
  return status;
}



/*****************************************************************
 * 3. ESL_DSQDATA_CHUNK: a chunk of input sequence data
 *****************************************************************/

static ESL_DSQDATA_CHUNK *
dsqdata_chunk_Create(ESL_DSQDATA *dd)
{
  ESL_DSQDATA_CHUNK *chu = NULL;
  int                U;               // max size of unpacked seq data, in bytes (smem allocation)
  int                status;

  ESL_ALLOC(chu, sizeof(ESL_DSQDATA_CHUNK));
  chu->i0       = 0;
  chu->N        = 0;
  chu->pn       = 0;
  chu->dsq      = NULL;
  chu->name     = NULL;
  chu->acc      = NULL;
  chu->desc     = NULL;
  chu->taxid    = NULL;
  chu->L        = NULL;
  chu->metadata = NULL;
  chu->smem     = NULL;
  chu->nxt      = NULL;

  /* dsq, name, acc, desc are arrays of pointers into smem, metadata.
   * taxid is cast to int, from the metadata.
   * L is figured out by the unpacker.
   * All of these are set by the unpacker.
   */
  ESL_ALLOC(chu->dsq,   dd->chunk_maxseq * sizeof(ESL_DSQ *));   
  ESL_ALLOC(chu->name,  dd->chunk_maxseq * sizeof(char *));
  ESL_ALLOC(chu->acc,   dd->chunk_maxseq * sizeof(char *));
  ESL_ALLOC(chu->desc,  dd->chunk_maxseq * sizeof(char *));
  ESL_ALLOC(chu->taxid, dd->chunk_maxseq * sizeof(int));
  ESL_ALLOC(chu->L,     dd->chunk_maxseq * sizeof(int64_t));

  /* On the <smem> allocation, and the <dsq> and <psq> pointers into it:
   *
   * <maxpacket> (in uint32's) sets the maximum single fread() size:
   * one load of a new chunk of packed sequence, up to maxpacket*4
   * bytes. <smem> needs to be able to hold both that and the fully
   * unpacked sequence, because we unpack in place.  Each packet
   * unpacks to at most 6 or 15 residues (5-bit or 2-bit packing) We
   * don't pack sentinels, so the maximum unpacked size includes
   * <maxseq>+1 sentinels... because we concat the digital seqs so
   * that the trailing sentinel of seq i is the leading sentinel of
   * seq i+1.
   *
   * The packed seq (max of P bytes) loads overlap with the unpacked
   * data (max of U bytes):
   *                   psq
   *                   v[    P bytes    ]
   * smem: 0........0........0..........0
   *       ^[         U bytes           ]
   *       ^dsq[0]  ^dsq[1]  ^dsq[2]
   *
   * and as long as we unpack psq left to right -- and as long as we
   * read the last packet before we write the last unpacked residues
   * to smem - we're guaranteed that the unpacking works without
   * overwriting any unpacked data.
   */
  U  = (dd->pack5 ? 6 * dd->chunk_maxpacket : 15 * dd->chunk_maxpacket);
  U += dd->chunk_maxseq + 1;
  ESL_ALLOC(chu->smem, sizeof(ESL_DSQ) * U);
  chu->psq = (uint32_t *) (chu->smem + U - 4*dd->chunk_maxpacket);

  /* We don't have any guarantees about the amount of metadata
   * associated with the N sequences, so <metadata> has to be a
   * reallocatable space. We make a lowball guess for the initial
   * alloc, on the off chance that the metadata size is small (names
   * only, no acc/desc): minimally, say 12 bytes of name, 3 \0's, and
   * 4 bytes for the taxid integer: call it 20.
   */
  chu->mdalloc = 20 * dd->chunk_maxseq;
  ESL_ALLOC(chu->metadata, sizeof(char) * chu->mdalloc);

  return chu;
  
 ERROR:
  dsqdata_chunk_Destroy(chu);
  return NULL;
}


static void
dsqdata_chunk_Destroy(ESL_DSQDATA_CHUNK *chu)
{
  if (chu)
    {
      if (chu->metadata) free(chu->metadata);
      if (chu->smem)     free(chu->smem);
      if (chu->L)        free(chu->L);
      if (chu->taxid)    free(chu->taxid);
      if (chu->desc)     free(chu->desc);
      if (chu->acc)      free(chu->acc);
      if (chu->name)     free(chu->name);
      if (chu->dsq)      free(chu->dsq);
      free(chu);
    }
}


/*****************************************************************
 * 4. Loader and unpacker, the input threads
 *****************************************************************/

static void *
dsqdata_loader_thread(void *p)
{
  ESL_DSQDATA         *dd        = (ESL_DSQDATA *) p;
  ESL_DSQDATA_RECORD  *idx       = NULL;
  ESL_DSQDATA_CHUNK   *chu       = NULL;
  int                  nchunk    = 0;             // number of chunks we create, and need to destroy.
  int                  nidx      = 0;             // how many records in <idx>: usually MAXSEQ, until end
  int                  nload     = 0;             // how many sequences we load: >=1, <=nidx
  int                  ncarried  = 0;             // how many records carry over to next iteration: nidx-nload
  int                  nread     = 0;             // fread()'s return value
  int                  nmeta     = 0;             // how many bytes of metadata we want to read for this chunk
  int                  i0        = 0;             // absolute index of first record in <idx>, 0-offset
  int64_t              psq_last  = -1;            // psq_end for record i0-1
  int64_t              meta_last = -1;            // metadata_end for record i0-1
  int                  done      = FALSE;
  int                  status;
  
  ESL_ALLOC(idx, sizeof(ESL_DSQDATA_RECORD) * dd->chunk_maxseq);

  while (! done)
    {

      /* Get a chunk - either by creating it, or recycling it.
       * We'll create up to <nconsumers>+2 of them.
       */
      if (nchunk < dd->nconsumers+2)
	{
	  if ( (chu = dsqdata_chunk_Create(dd)) == NULL) { status = eslEMEM; goto ERROR; }
	  nchunk++;
	}
      else
	{
	  if ( pthread_mutex_lock(&dd->recycling_mutex) != 0) ESL_XEXCEPTION(eslESYS, "pthread mutex lock failed");
	  while (dd->recycling == NULL)
	    {
	      if ( pthread_cond_wait(&dd->recycling_cv, &dd->recycling_mutex) != 0) 
		ESL_XEXCEPTION(eslESYS, "pthread cond wait failed");
	    }
	  chu = dd->recycling;
	  dd->recycling = chu->nxt;                    // pop one off recycling stack
	  if ( pthread_mutex_unlock(&dd->recycling_mutex) != 0) ESL_XEXCEPTION(eslESYS, "pthread mutex unlock failed");
	  if ( pthread_cond_signal(&dd->recycling_cv)     != 0) ESL_XEXCEPTION(eslESYS, "pthread cond signal failed");
	  // signal *after* unlocking mutex
	}
      
      /* Refill index. (The memmove is avoidable. Alt strategy: we could load in 2 frames)
       * The previous loop loaded packed sequence for <nload'> of the <nidx'> entries,
       * where the 's indicate the variable has carried over from prev iteration:
       *       |----- nload' ----||--- (ncarried) ---|
       *       |-------------- nidx' ----------------|
       * Now we're going to shift the remainder ncarried = nidx-nload to the left, then refill:
       *       |---- ncarried ----||--- (MAXSEQ-ncarried) ---|
       *       |-------------- MAXSEQ -----------------------|
       * while watching out for the terminal case where we run out of
       * data, loading less than (MAXSEQ-ncarried) records:
       *       |---- ncarried ----||--- nidx* ---|
       *       |------------- nidx --------------|
       * where the <nidx*> is what fread() returns to us.
       */
      i0      += nload;               // this chunk starts with seq #<i0>
      ncarried = (nidx - nload);
      memmove(idx, idx + nload, sizeof(ESL_DSQDATA_RECORD) * ncarried);
      nidx  = fread(idx + ncarried, sizeof(ESL_DSQDATA_RECORD), dd->chunk_maxseq - ncarried, dd->ifp);
      nidx += ncarried;               // usually, this'll be MAXSEQ, unless we're near EOF.
      
      if (nidx == 0) 
	{ // We're EOF. This chunk will be the empty EOF signal to unpacker, consumers.
	  chu->i0 = i0;
	  chu->N  = 0;
	  chu->pn = 0;
	  done    = TRUE;
	}
      else
	{
	  /* Figure out how many sequences we're going to load: <nload>
	   *  nload = max i : i <= MAXSEQ && idx[i].psq_end - psq_last <= CHUNK_MAX
	   */
	  ESL_DASSERT1(( idx[0].psq_end - psq_last <= dd->chunk_maxpacket ));
	  if (idx[nidx-1].psq_end - psq_last <= dd->chunk_maxpacket)
	    nload = nidx;
	  else
	    { // Binary search for nload = max_i idx[i-1].psq_end - lastend <= MAX
	      int righti = nidx;
	      int mid;
	      nload = 1;
	      while (righti - nload > 1)
		{
		  mid = nload + (righti - nload) / 2;
		  if (idx[mid-1].psq_end - psq_last <= dd->chunk_maxpacket) nload = mid;
		  else righti = mid;
		}                                                  
	    }
	  
	  /* Read packed sequence. */
	  chu->pn = idx[nload-1].psq_end - psq_last;
	  nread   = fread(chu->psq, sizeof(uint32_t), chu->pn, dd->sfp);
	  //printf("Read %d packed ints from seq file\n", nread);
	  if ( nread != chu->pn ) ESL_XEXCEPTION(eslEOD, "dsqdata packet loader: expected %d, got %d", chu->pn, nread);

	      
	  /* Read metadata, reallocating if needed */
	  nmeta = idx[nload-1].metadata_end - meta_last;
	  if (nmeta > chu->mdalloc) {
	    ESL_REALLOC(chu->metadata, sizeof(char) * nmeta);   // should be realloc by doubling instead?
	    chu->mdalloc = nmeta;
	  }
	  nread  = fread(chu->metadata, sizeof(char), nmeta, dd->mfp);
	  if ( nread != nmeta ) ESL_XEXCEPTION(eslEOD, "dsqdata metadata loader: expected %d, got %d", nmeta, nread); 

	  chu->i0   = i0;
	  chu->N    = nload;
	  psq_last  = idx[nload-1].psq_end;
	  meta_last = idx[nload-1].metadata_end;
	}

      /* Put the finished chunk into outbox;
       * unpacker will pick it up and unpack it.
       */
      if ( pthread_mutex_lock(&dd->loader_outbox_mutex) != 0) ESL_XEXCEPTION(eslESYS, "pthread mutex lock failed");
      while (dd->loader_outbox != NULL) 
	{ 
	  if (pthread_cond_wait(&dd->loader_outbox_empty_cv, &dd->loader_outbox_mutex) != 0)
	    ESL_XEXCEPTION(eslESYS, "pthread cond wait failed");
	}
      dd->loader_outbox = chu;   
      if ( pthread_mutex_unlock(&dd->loader_outbox_mutex)  != 0) ESL_XEXCEPTION(eslESYS, "pthread mutex unlock failed");
      if ( pthread_cond_signal(&dd->loader_outbox_full_cv) != 0) ESL_XEXCEPTION(eslESYS, "pthread cond signal failed");
    }
  /* done == TRUE: we've sent the empty EOF chunk downstream, and now
   * we wait to get all our chunks back through the recycling, so we
   * can free them and exit cleanly. We counted them as they went out,
   * in <nchunk>, so we know how many need to come home.
   */

  while (nchunk)
    {
      if ( pthread_mutex_lock(&dd->recycling_mutex) != 0) ESL_XEXCEPTION(eslESYS, "pthread mutex lock failed");
      while (dd->recycling == NULL)                 // Readers may still be working, will Recycle() their chunks
	{
	  if ( pthread_cond_wait(&dd->recycling_cv, &dd->recycling_mutex) != 0)
	    ESL_XEXCEPTION(eslESYS, "pthread cond wait failed");
	}
      while (dd->recycling != NULL) {               // Free entire stack, while we have the mutex locked.
	chu           = dd->recycling;   
	dd->recycling = chu->nxt;
	dsqdata_chunk_Destroy(chu);
	nchunk--;
      }
      if ( pthread_mutex_unlock(&dd->recycling_mutex) != 0) ESL_XEXCEPTION(eslESYS, "pthread mutex unlock failed");
      /* Because the recycling is a stack, readers never have to wait
       * on a condition to Recycle(); the recycling, unlike the
       * outboxes, doesn't need to be empty.
       */
    }
  free(idx);
  pthread_exit(NULL);


 ERROR: 
  /* Defying Easel standards, we treat all exceptions as fatal, at
   * least for the moment.  This isn't a problem in HMMER, Infernal
   * because they already use fatal exception handlers (i.e., we never
   * reach this code anyway, if the parent app is using default fatal
   * exception handling). It would become a problem if an Easel-based
   * app needs to assure no exits from within Easel. Because the other
   * threads will block waiting for chunks to come from the loader, if
   * the loader fails, we would need a back channel signal of some
   * sort to get the other threads to clean up and terminate.
   */
  if (idx) free(idx);    
  esl_fatal("  ... dsqdata loader thread failed: unrecoverable");
}



static void *
dsqdata_unpacker_thread(void *p)
{
  ESL_DSQDATA          *dd   = (ESL_DSQDATA *) p;
  ESL_DSQDATA_CHUNK    *chu  = NULL;
  int                   done = FALSE;
  int                   status;

  while (! done)
    {
      /* Get a chunk from loader's outbox. Wait if necessary. */
      if ( pthread_mutex_lock(&dd->loader_outbox_mutex) != 0) ESL_XEXCEPTION(eslESYS, "pthread mutex lock failed");
      while (dd->loader_outbox == NULL) 
	{
	  if ( pthread_cond_wait(&dd->loader_outbox_full_cv, &dd->loader_outbox_mutex) != 0)
	    ESL_XEXCEPTION(eslESYS, "pthread cond wait failed");
	}
      chu = dd->loader_outbox;
      dd->loader_outbox  = NULL;
      if ( pthread_mutex_unlock(&dd->loader_outbox_mutex)   != 0) ESL_XEXCEPTION(eslESYS, "pthread mutex unlock failed");
      if ( pthread_cond_signal(&dd->loader_outbox_empty_cv) != 0) ESL_XEXCEPTION(eslESYS, "pthread cond signal failed");

      /* Unpack the metadata.
       * If chunk is empty (N==0), it's the EOF signal - let it go straight out to a consumer.
       * (The first consumer that sees it will set the at_eof flag in <dd>, which all
       *  consumers check. So we only need the one empty EOF chunk to flow downstream.)
       */
      if (! chu->N) done = TRUE; // still need to pass the chunk along to a consumer.
      else
	{ 	
	  if (( status = dsqdata_unpack_chunk(chu)) != eslOK) goto ERROR;
	}

      /* Put unpacked chunk into the unpacker's outbox.
       * May need to wait for it to be empty/available.
       */
      if ( pthread_mutex_lock(&dd->unpacker_outbox_mutex) != 0) ESL_XEXCEPTION(eslESYS, "pthread mutex lock failed");
      while (dd->unpacker_outbox != NULL) 
	{ 
	  if ( pthread_cond_wait(&dd->unpacker_outbox_empty_cv, &dd->unpacker_outbox_mutex) != 0)
	    ESL_XEXCEPTION(eslESYS, "pthread cond wait failed");
	}
      dd->unpacker_outbox = chu;
      if ( pthread_mutex_unlock(&dd->unpacker_outbox_mutex)  != 0) ESL_XEXCEPTION(eslESYS, "pthread mutex unlock failed");
      if ( pthread_cond_signal(&dd->unpacker_outbox_full_cv) != 0) ESL_XEXCEPTION(eslESYS, "pthread cond signal failed");
    }
  pthread_exit(NULL);

 ERROR:
  /* See comment in loader thread: for lack of a back channel mechanism
   * to tell other threads to clean up and terminate, we violate Easel standards
   * and turn nonfatal exceptions into fatal ones.
   */
  esl_fatal("  ... dsqdata unpacker thread failed: unrecoverable"); 
}


/*****************************************************************
 * 5. Packing sequences and unpacking chunks
 *****************************************************************/

/* dsqdata_unpack_chunk()
 *
 * Throws:    <eslEFORMAT> if a problem is seen in the binary format 
 */
static int
dsqdata_unpack_chunk(ESL_DSQDATA_CHUNK *chu)
{
  char     *ptr = chu->metadata;           // ptr will walk through metadata
  ESL_DSQ  *dsq = (ESL_DSQ *) chu->smem;   // concatenated unpacked digital seq as one array
  int       r   = 0;                       // position in unpacked dsq array
  int       i   = 0;                       // sequence index: 0..chu->N-1
  int       pos;                           // position in packet array
  uint32_t  v;                             // one packet, picked up
  int       bitshift;                      
  
  /* "Unpack" the metadata */
  for (i = 0; i < chu->N; i++)
    {
      /* The data are user input, so we cannot trust that it has \0's where we expect them.  */
      if ( ptr >= chu->metadata + chu->mdalloc) ESL_EXCEPTION(eslEFORMAT, "metadata format error");
      chu->name[i] = ptr;                           ptr = 1 + strchr(ptr, '\0');   if ( ptr >= chu->metadata + chu->mdalloc) ESL_EXCEPTION(eslEFORMAT, "metadata format error");
      chu->acc[i]  = ptr;                           ptr = 1 + strchr(ptr, '\0');   if ( ptr >= chu->metadata + chu->mdalloc) ESL_EXCEPTION(eslEFORMAT, "metadata format error");
      chu->desc[i] = ptr;                           ptr = 1 + strchr(ptr, '\0');   if ( ptr >= chu->metadata + chu->mdalloc) ESL_EXCEPTION(eslEFORMAT, "metadata format error");
      chu->taxid[i] = (int32_t) *((int32_t *) ptr); ptr += sizeof(int32_t);     
    }

  /* Unpack the sequence data */
  i = 0;
  chu->dsq[0] = (ESL_DSQ *) chu->smem;
  dsq[r++]    = eslDSQ_SENTINEL;
  for (pos = 0; pos < chu->pn; pos++)
    {
      v = chu->psq[pos];  // Must pick up, because of packed psq overlap w/ unpacked smem
      
      /* Look at the two packet control bits together.
       * 00 = 2bit full packet
       * 01 = 5bit full packet. 
       * 10 = 2bit EOD packet (must be full)
       * 11 = 5bit EOD packet.
       */
      switch ( (v >> 30) ) {
      case 0: 
	dsq[r++] = (v >> 28) & 3;  dsq[r++] = (v >> 26) & 3;  dsq[r++] = (v >> 24) & 3;
	dsq[r++] = (v >> 22) & 3;  dsq[r++] = (v >> 20) & 3;  dsq[r++] = (v >> 18) & 3;
	dsq[r++] = (v >> 16) & 3;  dsq[r++] = (v >> 14) & 3;  dsq[r++] = (v >> 12) & 3;
	dsq[r++] = (v >> 10) & 3;  dsq[r++] = (v >>  8) & 3;  dsq[r++] = (v >>  6) & 3;
	dsq[r++] = (v >>  4) & 3;  dsq[r++] = (v >>  2) & 3;  dsq[r++] =         v & 3;
	break;

      case 1:
	dsq[r++] = (v >> 25) & 31; dsq[r++] = (v >> 20) & 31; dsq[r++] = (v >> 15) & 31;
	dsq[r++] = (v >> 10) & 31; dsq[r++] = (v >>  5) & 31; dsq[r++] = (v >>  0) & 31;
	break;	

      case 2:
	dsq[r++] = (v >> 28) & 3;  dsq[r++] = (v >> 26) & 3;  dsq[r++] = (v >> 24) & 3;
	dsq[r++] = (v >> 22) & 3;  dsq[r++] = (v >> 20) & 3;  dsq[r++] = (v >> 18) & 3;
	dsq[r++] = (v >> 16) & 3;  dsq[r++] = (v >> 14) & 3;  dsq[r++] = (v >> 12) & 3;
	dsq[r++] = (v >> 10) & 3;  dsq[r++] = (v >>  8) & 3;  dsq[r++] = (v >>  6) & 3;
	dsq[r++] = (v >>  4) & 3;  dsq[r++] = (v >>  2) & 3;  dsq[r++] =         v & 3;

	chu->L[i] = &(dsq[r]) - chu->dsq[i] - 1;
	i++;
	if (i < chu->N) chu->dsq[i] = &(dsq[r]);
	dsq[r++] = eslDSQ_SENTINEL;
	break;

      case 3:
	// In 5-bit EOD packets we have to stop on internal sentinel.
	for (bitshift = 25; bitshift >= 0 && ((v >> bitshift) & 31) != 31; bitshift -= 5)
	  dsq[r++] = (v >> bitshift) & 31;
	chu->L[i] = &(dsq[r]) - chu->dsq[i] - 1;
	i++;
	if (i < chu->N) chu->dsq[i] = &(dsq[r]);
	dsq[r++] = eslDSQ_SENTINEL;
	break;
      }
    }
   ESL_DASSERT1(( i == chu->N ));
   return eslOK;
}


/* dsqdata_pack5()
 *
 * Pack a digital sequence <dsq> of length <n>, in place.  The packet
 * array <*ret_psq>, <*ret_plen> packets long, uses the same memory
 * and <dsq> is overwritten. No allocation is needed; we know that
 * <dsq> is longer than <psq> and we don't care if we overwrite <dsq>.
 * 
 * It's possible to have n==0, for a 0 length digital sequence; in that 
 * case you'll get plen=0.
 */
static int
dsqdata_pack5(ESL_DSQ *dsq, int n, uint32_t **ret_psq, int *ret_plen)
{
  uint32_t  *psq      = (uint32_t *) dsq;  // packing in place
  int        r        = 1;                 // position in <dsq>
  int        pos      = 0;                 // position in <psq>. 
  int        b;                            // bitshift
  uint32_t   v;

  while (r <= n)
    {
      v = (1 << 30);             // initialize packet with 5-bit flag
      for (b = 25; b >= 0 && r <= n; b -= 5)  v  |= (uint32_t) dsq[r++] << b;
      for (      ; b >= 0;           b -= 5)  v  |= (uint32_t)       31 << b;

      if (r > n) v |= (1 << 31); // EOD bit
      psq[pos++] = v;            // we know we've already read all the dsq we need under psq[pos]
    }

  *ret_psq  = psq;
  *ret_plen = pos;
  return eslOK;
}


/* dsqdata_pack2()
 *
 * Pack <dsq> in place. 
 * Saves worrying about allocation for packed seq buffer.
 * *ret_psq is the same memory as dsq, with the packets flush left.
 * <dsq> is destroyed by packing.
 */
static int
dsqdata_pack2(ESL_DSQ *dsq, int n, uint32_t **ret_psq, int *ret_plen)
{
  uint32_t  *psq      = (uint32_t *) dsq; // packing in place
  int        pos      = 0;                // position in <psq>
  int        d        = 0;                // position of next degen residue, 1..n, n+1 if none
  int        r        = 1;                // position in <dsq> 1..n
  int        b;                           // bitshift
  uint32_t   v;

  while (r <= n)
    {
      // Slide the "next degenerate residue" detector
      if (d < r)
	for (d = r; d <= n; d++)
	  if (dsq[d] > 3) break;

      // Can we 2-bit pack the next 15 residues, r..r+14?
      // n-r+1 = number of residues remaining to be packed.
      if (n-r+1 >= 15 && d > r+14)
	{
	  v  = 0;
	  for (b = 28; b >= 0; b -=2) v |= (uint32_t) dsq[r++] << b;
	}
      else
	{
	  v = (1 << 30); // initialize v with 5-bit packing bit
	  for (b = 25; b >= 0 && r <= n; b -= 5) v  |= (uint32_t) dsq[r++] << b;
	  for (      ; b >= 0;           b -= 5) v  |= (uint32_t)       31 << b;
	}

      if (r > n) v |= (1 << 31); // EOD bit
      psq[pos++] = v;            // we know we've already read all the dsq we need under psq[pos]
    }
  
  *ret_psq  = psq;
  *ret_plen = pos;
  return eslOK;
}


/*****************************************************************
 * 6. Notes
 ***************************************************************** 
 *
 * [1] Packed sequence data format.
 * 
 *      Format of a single packet:
 *      [31] [30] [29..25]  [24..20]  [19..15]  [14..10]  [ 9..5 ]  [ 4..0 ]
 *       ^    ^   |------------  6 5-bit packed residues ------------------|
 *       |    |   []  []  []  []  []  []  []  []  []  []  []  []  []  []  []
 *       |    |   |----------- or 15 2-bit packed residues ----------------|
 *       |    |    
 *       |    "packtype" bit 30 = 0 if packet is 2-bit packed; 1 if 5-bit packed
 *       "sentinel" bit 31 = 1 if last packet in packed sequence; else 0
 *       
 *       (packet & (1 << 31)) tests for end of sequence
 *       (packet & (1 << 30)) tests for 5-bit packing vs. 2-bit
 *       ((packet >> shift) && 31) decodes 5-bit, for shift=25..0 in steps of 5
 *       ((packet >> shift) && 3)  decodes 2-bit, for shift=28..0 in steps of 2
 *       
 *       Packets without the sentinel bit set are always full (unpack
 *       to 15 or 6 residue codes).
 *       
 *       5-bit EOD packets may be partial: they unpack to 1..6
 *       residues.  The remaining residue codes are set to 0x1f
 *       (11111) to indicate EOD within a partial packet.
 *       
 *       2-bit EOD packets must be full, because there is no way to
 *       signal EOD locally within a 2-bit packet. Can't use 0x03 (11)
 *       because that's T/U. Generally, then, the last packet of a
 *       nucleic acid sequence must be 5-bit encoded, solely to be
 *       able to encode EOD in a partial packet. 
 *  
 *       A protein sequence of length N packs into exactly (N+5)/6
 *       5-bit packets. A DNA sequence packs into <= (N+14)/15 mixed
 *       2- and 5-bit packets.
 *       
 *       A packed sequence consists of an integer number of packets,
 *       P, ending with an EOD packet that may contain a partial
 *       number of residues.
 *       
 *       A packed amino acid sequence unpacks to <= 6P residues, and
 *       all packets are 5-bit encoded.
 *       
 *       A packed nucleic acid sequence unpacks to <= 15P residues.
 *       The packets are a mix of 2-bit and 5-bit. Degenerate residues
 *       must be 5-bit packed, and the EOD packet usually is too. A
 *       5-bit packet does not have to contain degenerate residues,
 *       because it may have been necessary to get "in frame" to pack
 *       a downstream degenerate residue. For example, the sequence
 *       ACGTACGTNNA... must be packed as [ACGTAC][CGTNNA]... to get
 *       the N's packed correctly.
 *       
 * [2] Compression: relative incompressibility of biological sequences.
 *
 *      Considered using fast (de)compression algorithms that are fast
 *      enough to keep up with disk read speed, including LZ4 and
 *      Google's Snappy. However, lz4 only achieves 1.0-1.9x global
 *      compression of protein sequence (compared to 1.5x for
 *      packing), and 2.0x for DNA (compared to 3.75x for packing).
 *      With local, blockwise compression, which we need for random
 *      access and indexing, it gets worse. Packing is superior.
 *      
 *      Metadata compression is more feasible, but I still opted
 *      against it. Although metadata are globally quite compressible
 *      (3.2-6.9x in trials with lz4), locally in 64K blocks lz4 only
 *      achieves 2x.  [xref SRE:2016/0201-seqfile-compression]
 *      
 * [3] Maybe getting more packing using run-length encoding.
 * 
 *      Genome assemblies typically have long runs of N's (human
 *      GRCh38.p2 is about 5% N), and it's excruciating to have to
 *      pack it into bulky 5-bit degenerate packets. I considered
 *      run-length encoding (RLE). One possibility is to use a special
 *      packet format akin to the 5-bit packet format:
 *      
 *        [0] [?] [11111] [.....] [....................]
 *        ^        ^       ^       20b number, <=2^20-1
 *        |        |       5-bit residue code       
 *        |        sentinel residue 31 set
 *        sentinel bit unset
 *        
 *      This is a uniquely detectable packet structure because a full
 *      packet (with unset sentinel bit) would otherwise never contain
 *      a sentinel residue (code 31).
 *      
 *      However, using RLE would make our unpacked data sizes too
 *      unpredictable; we wouldn't have the <=6P or <=15P guarantee,
 *      so we couldn't rely on fixed-length allocation of <smem> in
 *      our chunk. Consumers wouldn't be getting predictable chunk
 *      sizes, which could complicate load balancing. I decided
 *      against it.
 */


/*****************************************************************
 * 7. Unit tests
 *****************************************************************/

/* Exercise the packing and unpacking routines:
 *    dsqdata_pack2, dsqdata_pack5, and dsqdata_unpack
 */
//static void
//utest_packing(ESL_ALPHABET *abc)



/*****************************************************************
 * 8. Test driver
 *****************************************************************/



/*****************************************************************
 * 9. Examples
 *****************************************************************/

/* esl_dsqdata_example2
 * Example of creating a new dsqdata database from a sequence file.
 */
#ifdef eslDSQDATA_EXAMPLE2
#include "easel.h"
#include "esl_alphabet.h"
#include "esl_dsqdata.h"
#include "esl_getopts.h"

static ESL_OPTIONS options[] = {
  /* name           type      default  env  range toggles reqs incomp  help                                       docgroup*/
  { "-h",        eslARG_NONE,   FALSE,  NULL, NULL,  NULL,  NULL, NULL, "show brief help on version and usage",    0 },
  { "--dna",     eslARG_NONE,   FALSE,  NULL, NULL,  NULL,  NULL, NULL, "use DNA alphabet",                        0 },
  { "--rna",     eslARG_NONE,   FALSE,  NULL, NULL,  NULL,  NULL, NULL, "use RNA alphabet",                        0 },
  { "--amino",   eslARG_NONE,   FALSE,  NULL, NULL,  NULL,  NULL, NULL, "use protein alphabet",                    0 },
  {  0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
};
static char usage[]  = "[-options] <seqfile_in> <binary seqfile_out>";
static char banner[] = "experimental: create binary database for esl_dsqdata";

int
main(int argc, char **argv)
{
  ESL_GETOPTS    *go        = esl_getopts_CreateDefaultApp(options, 2, argc, argv, banner, usage);
  ESL_ALPHABET   *abc       = NULL;
  char           *infile    = esl_opt_GetArg(go, 1);
  char           *basename  = esl_opt_GetArg(go, 2);
  int             format    = eslSQFILE_UNKNOWN;
  int             alphatype = eslUNKNOWN;
  ESL_SQFILE     *sqfp      = NULL;
  char            errbuf[eslERRBUFSIZE];
  int             status;

  status = esl_sqfile_Open(infile, format, NULL, &sqfp);
  if      (status == eslENOTFOUND) esl_fatal("No such file.");
  else if (status == eslEFORMAT)   esl_fatal("Format unrecognized.");
  else if (status != eslOK)        esl_fatal("Open failed, code %d.", status);

  if      (esl_opt_GetBoolean(go, "--rna"))   alphatype = eslRNA;
  else if (esl_opt_GetBoolean(go, "--dna"))   alphatype = eslDNA;
  else if (esl_opt_GetBoolean(go, "--amino")) alphatype = eslAMINO;
  else {
    status = esl_sqfile_GuessAlphabet(sqfp, &alphatype);
    if      (status == eslENOALPHABET) esl_fatal("Couldn't guess alphabet from first sequence in %s", infile);
    else if (status == eslEFORMAT)     esl_fatal("Parse failed (sequence file %s)\n%s\n", infile, sqfp->get_error(sqfp));     
    else if (status == eslENODATA)     esl_fatal("Sequence file %s contains no data?", infile);
    else if (status != eslOK)          esl_fatal("Failed to guess alphabet (error code %d)\n", status);
  }
  abc = esl_alphabet_Create(alphatype);
  esl_sqfile_SetDigital(sqfp, abc);

  status = esl_dsqdata_Write(sqfp, basename, errbuf);
  if      (status == eslEWRITE)  esl_fatal("Failed to open dsqdata output files:\n  %s", errbuf);
  else if (status == eslEFORMAT) esl_fatal("Parse failed (sequence file %s)\n  %s", infile, sqfp->get_error(sqfp));
  else if (status != eslOK)      esl_fatal("Unexpected error while creating dsqdata file (code %d)\n", status);

  esl_sqfile_Close(sqfp);
  esl_alphabet_Destroy(abc);
  esl_getopts_Destroy(go);
  return eslOK;
}
#endif /*eslDSQDATA_EXAMPLE2*/


/* esl_dsqdata_example
 * Example of opening and reading a dsqdata database.
 */
#ifdef eslDSQDATA_EXAMPLE
#include "easel.h"
#include "esl_alphabet.h"
#include "esl_dsqdata.h"
#include "esl_getopts.h"

static ESL_OPTIONS options[] = {
  /* name             type          default  env  range toggles reqs incomp  help                                       docgroup*/
  { "-h",          eslARG_NONE,       FALSE,  NULL, NULL,  NULL,  NULL, NULL, "show brief help on version and usage",        0 },
  { "-n",          eslARG_NONE,       FALSE,  NULL, NULL,  NULL,  NULL, NULL, "no residue counting: faster time version",    0 },
  {  0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
};

static char usage[]  = "[-options] <basename>";
static char banner[] = "example of using ESL_DSQDATA to read sequence db";

int
main(int argc, char **argv)
{
  ESL_GETOPTS       *go       = esl_getopts_CreateDefaultApp(options, 1, argc, argv, banner, usage);
  ESL_ALPHABET      *abc      = NULL;
  char              *basename = esl_opt_GetArg(go, 1);
  int                no_count = esl_opt_GetBoolean(go, "-n");
  ESL_DSQDATA       *dd       = NULL;
  ESL_DSQDATA_CHUNK *chu      = NULL;
  int                i;
  int64_t            pos;
  int64_t            ct[128], total;
  int                x;
  int                ncpu     = 4;
  int                status;
  
  status = esl_dsqdata_Open(&abc, basename, ncpu, &dd);
  if      (status == eslENOTFOUND) esl_fatal("Failed to open dsqdata files:\n  %s",    dd->errbuf);
  else if (status == eslEFORMAT)   esl_fatal("Format problem in dsqdata files:\n  %s", dd->errbuf);
  else if (status != eslOK)        esl_fatal("Unexpected error in opening dsqdata (code %d)", status);

  for (x = 0; x < 127; x++) ct[x] = 0;

  while ((status = esl_dsqdata_Read(dd, &chu)) != eslEOF)
    {
      if (! no_count)
	for (i = 0; i < chu->N; i++)
	  for (pos = 1; pos <= chu->L[i]; pos++)
	    ct[ chu->dsq[i][pos] ]++;

      esl_dsqdata_Recycle(dd, chu);
    }
  if (status != eslEOF) esl_fatal("unexpected error %d in reading dsqdata", status);

  if (! no_count)
    {
      total = 0;
      for (x = 0; x < abc->Kp; x++) 
	{
	  printf("%c  %" PRId64 "\n", abc->sym[x], ct[x]);
	  total += ct[x];
	}
      printf("Total = %" PRId64 "\n", total);
    }

  esl_alphabet_Destroy(abc);
  esl_dsqdata_Close(dd);
  esl_getopts_Destroy(go);
  return 0;
}
#endif /*eslDSQDATA_EXAMPLE*/



