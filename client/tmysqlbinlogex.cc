/*
   Copyright (c) 2000, 2011, Oracle and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA
*/

/* 

   TODO: print the catalog (some USE catalog.db ????).

   Standalone program to read a MySQL binary log (or relay log).

   Should be able to read any file of these categories, even with
   --start-position.
   An important fact: the Format_desc event of the log is at most the 3rd event
   of the log; if it is the 3rd then there is this combination:
   Format_desc_of_slave, Rotate_of_master, Format_desc_of_master.
*/

#define MYSQL_CLIENT
#undef MYSQL_SERVER
#include "client_priv.h"
#include <my_time.h>
/* That one is necessary for defines of OPTION_NO_FOREIGN_KEY_CHECKS etc */
#include "sql_priv.h"
#include "log_event.h"
#include "sql_common.h"
#include <welcome_copyright_notice.h> // ORACLE_WELCOME_COPYRIGHT_NOTICE

#define BIN_LOG_HEADER_SIZE	4
#define PROBE_HEADER_LEN	(EVENT_LEN_OFFSET+4)


#define CLIENT_CAPABILITIES	(CLIENT_LONG_PASSWORD | CLIENT_LONG_FLAG | CLIENT_LOCAL_FILES)

char server_version[SERVER_VERSION_LENGTH];
ulong server_id = 0;

// needed by net_serv.c
ulong bytes_sent = 0L, bytes_received = 0L;
ulong mysqld_net_retry_count = 10L;
ulong open_files_limit;
uint test_flags = 0; 
static uint opt_protocol= 0;


#ifndef DBUG_OFF
static const char* default_dbug_option = "d:t:o,/tmp/mysqlbinlog.trace";
#endif
static const char *load_default_groups[]= { "mysqlbinlog","client",0 };

static void error(const char *format, ...) ATTRIBUTE_FORMAT(printf, 1, 2);
static void warning(const char *format, ...) ATTRIBUTE_FORMAT(printf, 1, 2);

static bool one_database=0, to_last_remote_log= 0, disable_log_bin= 0;
static bool opt_hexdump= 0;
const char *base64_output_mode_names[]=
{"NEVER", "AUTO", "ALWAYS", "UNSPEC", "DECODE-ROWS", NullS};
TYPELIB base64_output_mode_typelib=
  { array_elements(base64_output_mode_names) - 1, "",
    base64_output_mode_names, NULL };
static enum_base64_output_mode opt_base64_output_mode= BASE64_OUTPUT_UNSPEC;
static char *opt_base64_output_mode_str= NullS;
static char* database= 0;
static my_bool force_opt= 0, short_form= 0, remote_opt= 0;
static my_bool debug_info_flag, debug_check_flag;
static my_bool force_if_open_opt= 1;
static ulonglong offset = 0;
static char* host = 0;
static int port= 0;
static uint my_end_arg;
static const char* sock= 0;
static char *opt_plugin_dir= 0, *opt_default_auth= 0;

#ifdef HAVE_SMEM
static char *shared_memory_base_name= 0;
#endif
static char* user = 0;
static char* pass = 0;
static char *charset= 0;

static uint verbose= 0;

static ulonglong start_position, stop_position;
#define start_position_mot ((my_off_t)start_position)
#define stop_position_mot  ((my_off_t)stop_position)

static char *start_datetime_str, *stop_datetime_str;
static my_time_t start_datetime= 0, stop_datetime= MY_TIME_T_MAX;
static ulonglong rec_count= 0;
static short binlog_flags = 0; 
static MYSQL* mysql = NULL;
static char* dirname_for_local_load= 0;

static uint concurrency = 0;
static char* remote_user = 0;
static char* remote_pass = 0;
static char* remote_host = 0;
static int remote_port = 0;
static char* table_split = 0;
static my_bool exit_when_error = FALSE;
static char* sql_files_output_dir = 0; 
static char* mysql_path = "mysql";
static my_bool write_to_file_only_flag = FALSE;

/**
  Pointer to the Format_description_log_event of the currently active binlog.

  This will be changed each time a new Format_description_log_event is
  found in the binlog. It is finally destroyed at program termination.
*/
static Format_description_log_event* glob_description_event= NULL;

/**
  Exit status for functions in this file.
*/
enum Exit_status {
  /** No error occurred and execution should continue. */
  OK_CONTINUE= 0,
  /** An error occurred and execution should stop. */
  ERROR_STOP,
  /** No error occurred but execution should stop. */
  OK_STOP
};

static Exit_status dump_local_log_entries(PRINT_EVENT_INFO *print_event_info,
                                          const char* logname);
//static Exit_status dump_remote_log_entries(PRINT_EVENT_INFO *print_event_info,
//                                           const char* logname);
static Exit_status dump_log_entries(const char* logname);
static Exit_status safe_connect();


class Load_log_processor
{
  char target_dir_name[FN_REFLEN];
  size_t target_dir_name_len;

  /*
    When we see first event corresponding to some LOAD DATA statement in
    binlog, we create temporary file to store data to be loaded.
    We add name of this file to file_names array using its file_id as index.
    If we have Create_file event (i.e. we have binary log in pre-5.0.3
    format) we also store save event object to be able which is needed to
    emit LOAD DATA statement when we will meet Exec_load_data event.
    If we have Begin_load_query event we simply store 0 in
    File_name_record::event field.
  */
  struct File_name_record
  {
    char *fname;
    Create_file_log_event *event;
  };
  /*
    @todo Should be a map (e.g., a hash map), not an array.  With the
    present implementation, the number of elements in this array is
    about the number of files loaded since the server started, which
    may be big after a few years.  We should be able to use existing
    library data structures for this. /Sven
  */
  DYNAMIC_ARRAY file_names;

  /**
    Looks for a non-existing filename by adding a numerical suffix to
    the given base name, creates the generated file, and returns the
    filename by modifying the filename argument.

    @param[in,out] filename Base filename

    @param[in,out] file_name_end Pointer to last character of
    filename.  The numerical suffix will be written to this position.
    Note that there must be a least five bytes of allocated memory
    after file_name_end.

    @retval -1 Error (can't find new filename).
    @retval >=0 Found file.
  */
  File create_unique_file(char *filename, char *file_name_end)
    {
      File res;
      /* If we have to try more than 1000 times, something is seriously wrong */
      for (uint version= 0; version<1000; version++)
      {
	sprintf(file_name_end,"-%x",version);
	if ((res= my_create(filename,0,
			    O_CREAT|O_EXCL|O_BINARY|O_WRONLY,MYF(0)))!=-1)
	  return res;
      }
      return -1;
    }

public:
  Load_log_processor() {}
  ~Load_log_processor() {}

  int init()
  {
    return init_dynamic_array(&file_names, sizeof(File_name_record),
			      100, 100);
  }

  void init_by_dir_name(const char *dir)
    {
      target_dir_name_len= (convert_dirname(target_dir_name, dir, NullS) -
			    target_dir_name);
    }
  void init_by_cur_dir()
    {
      if (my_getwd(target_dir_name,sizeof(target_dir_name),MYF(MY_WME)))
	exit(1);
      target_dir_name_len= strlen(target_dir_name);
    }
  void destroy()
  {
    File_name_record *ptr= (File_name_record *)file_names.buffer;
    File_name_record *end= ptr + file_names.elements;
    for (; ptr < end; ptr++)
    {
      if (ptr->fname)
      {
        my_free(ptr->fname);
        delete ptr->event;
        bzero((char *)ptr, sizeof(File_name_record));
      }
    }

    delete_dynamic(&file_names);
  }

  /**
    Obtain Create_file event for LOAD DATA statement by its file_id
    and remove it from this Load_log_processor's list of events.

    Checks whether we have already seen a Create_file_log_event with
    the given file_id.  If yes, returns a pointer to the event and
    removes the event from array describing active temporary files.
    From this moment, the caller is responsible for freeing the memory
    occupied by the event.

    @param[in] file_id File id identifying LOAD DATA statement.

    @return Pointer to Create_file_log_event, or NULL if we have not
    seen any Create_file_log_event with this file_id.
  */
  Create_file_log_event *grab_event(uint file_id)
    {
      File_name_record *ptr;
      Create_file_log_event *res;

      if (file_id >= file_names.elements)
        return 0;
      ptr= dynamic_element(&file_names, file_id, File_name_record*);
      if ((res= ptr->event))
        bzero((char *)ptr, sizeof(File_name_record));
      return res;
    }

  /**
    Obtain file name of temporary file for LOAD DATA statement by its
    file_id and remove it from this Load_log_processor's list of events.

    @param[in] file_id Identifier for the LOAD DATA statement.

    Checks whether we have already seen Begin_load_query event for
    this file_id. If yes, returns the file name of the corresponding
    temporary file and removes the filename from the array of active
    temporary files.  From this moment, the caller is responsible for
    freeing the memory occupied by this name.

    @return String with the name of the temporary file, or NULL if we
    have not seen any Begin_load_query_event with this file_id.
  */
  char *grab_fname(uint file_id)
    {
      File_name_record *ptr;
      char *res= 0;

      if (file_id >= file_names.elements)
        return 0;
      ptr= dynamic_element(&file_names, file_id, File_name_record*);
      if (!ptr->event)
      {
        res= ptr->fname;
        bzero((char *)ptr, sizeof(File_name_record));
      }
      return res;
    }
  Exit_status process(Create_file_log_event *ce);
  Exit_status process(Begin_load_query_log_event *ce);
  Exit_status process(Append_block_log_event *ae);
  File prepare_new_file_for_old_format(Load_log_event *le, char *filename);
  Exit_status load_old_format_file(NET* net, const char *server_fname,
                                   uint server_fname_len, File file);
  Exit_status process_first_event(const char *bname, size_t blen,
                                  const uchar *block,
                                  size_t block_len, uint file_id,
                                  Create_file_log_event *ce);
};


/**
  Creates and opens a new temporary file in the directory specified by previous call to init_by_dir_name() or init_by_cur_dir().

  @param[in] le The basename of the created file will start with the
  basename of the file pointed to by this Load_log_event.

  @param[out] filename Buffer to save the filename in.

  @return File handle >= 0 on success, -1 on error.
*/
File Load_log_processor::prepare_new_file_for_old_format(Load_log_event *le,
							 char *filename)
{
  size_t len;
  char *tail;
  File file;
  
  fn_format(filename, le->fname, target_dir_name, "", MY_REPLACE_DIR);
  len= strlen(filename);
  tail= filename + len;
  
  if ((file= create_unique_file(filename,tail)) < 0)
  {
    error("Could not construct local filename %s.",filename);
    return -1;
  }
  
  le->set_fname_outside_temp_buf(filename,len+(uint) strlen(tail));
  
  return file;
}


/**
  Reads a file from a server and saves it locally.

  @param[in,out] net The server to read from.

  @param[in] server_fname The name of the file that the server should
  read.

  @param[in] server_fname_len The length of server_fname.

  @param[in,out] file The file to write to.

  @retval ERROR_STOP An error occurred - the program should terminate.
  @retval OK_CONTINUE No error, the program should continue.
*/
Exit_status Load_log_processor::load_old_format_file(NET* net,
                                                     const char*server_fname,
                                                     uint server_fname_len,
                                                     File file)
{
  uchar buf[FN_REFLEN+1];
  buf[0] = 0;
  memcpy(buf + 1, server_fname, server_fname_len + 1);
  if (my_net_write(net, buf, server_fname_len +2) || net_flush(net))
  {
    error("Failed requesting the remote dump of %s.", server_fname);
    return ERROR_STOP;
  }
  
  for (;;)
  {
    ulong packet_len = my_net_read(net);
    if (packet_len == 0)
    {
      if (my_net_write(net, (uchar*) "", 0) || net_flush(net))
      {
        error("Failed sending the ack packet.");
        return ERROR_STOP;
      }
      /*
	we just need to send something, as the server will read but
	not examine the packet - this is because mysql_load() sends 
	an OK when it is done
      */
      break;
    }
    else if (packet_len == packet_error)
    {
      error("Failed reading a packet during the dump of %s.", server_fname);
      return ERROR_STOP;
    }
    
    if (packet_len > UINT_MAX)
    {
      error("Illegal length of packet read from net.");
      return ERROR_STOP;
    }
    if (my_write(file, (uchar*) net->read_pos, 
		 (uint) packet_len, MYF(MY_WME|MY_NABP)))
      return ERROR_STOP;
  }
  
  return OK_CONTINUE;
}


/**
  Process the first event in the sequence of events representing a
  LOAD DATA statement.

  Creates a temporary file to be used in LOAD DATA and writes first
  block of data to it. Registers its file name (and optional
  Create_file event) in the array of active temporary files.

  @param bname Base name for temporary file to be created.
  @param blen Base name length.
  @param block First block of data to be loaded.
  @param block_len First block length.
  @param file_id Identifies the LOAD DATA statement.
  @param ce Pointer to Create_file event object if we are processing
  this type of event.

  @retval ERROR_STOP An error occurred - the program should terminate.
  @retval OK_CONTINUE No error, the program should continue.
*/
Exit_status Load_log_processor::process_first_event(const char *bname,
                                                    size_t blen,
                                                    const uchar *block,
                                                    size_t block_len,
                                                    uint file_id,
                                                    Create_file_log_event *ce)
{
  uint full_len= target_dir_name_len + blen + 9 + 9 + 1;
  Exit_status retval= OK_CONTINUE;
  char *fname, *ptr;
  File file;
  File_name_record rec;
  DBUG_ENTER("Load_log_processor::process_first_event");

  if (!(fname= (char*) my_malloc(full_len,MYF(MY_WME))))
  {
    error("Out of memory.");
    delete ce;
    DBUG_RETURN(ERROR_STOP);
  }

  memcpy(fname, target_dir_name, target_dir_name_len);
  ptr= fname + target_dir_name_len;
  memcpy(ptr,bname,blen);
  ptr+= blen;
  ptr+= sprintf(ptr, "-%x", file_id);

  if ((file= create_unique_file(fname,ptr)) < 0)
  {
    error("Could not construct local filename %s%s.",
          target_dir_name,bname);
    my_free(fname);
    delete ce;
    DBUG_RETURN(ERROR_STOP);
  }

  rec.fname= fname;
  rec.event= ce;

  /*
     fname is freed in process_event()
     after Execute_load_query_log_event or Execute_load_log_event
     will have been processed, otherwise in Load_log_processor::destroy()
  */
  if (set_dynamic(&file_names, (uchar*)&rec, file_id))
  {
    error("Out of memory.");
    my_free(fname);
    delete ce;
    DBUG_RETURN(ERROR_STOP);
  }

  if (ce)
    ce->set_fname_outside_temp_buf(fname, (uint) strlen(fname));

  if (my_write(file, (uchar*)block, block_len, MYF(MY_WME|MY_NABP)))
  {
    error("Failed writing to file.");
    retval= ERROR_STOP;
  }
  if (my_close(file, MYF(MY_WME)))
  {
    error("Failed closing file.");
    retval= ERROR_STOP;
  }
  DBUG_RETURN(retval);
}


/**
  Process the given Create_file_log_event.

  @see Load_log_processor::process_first_event(const char*,uint,const char*,uint,uint,Create_file_log_event*)

  @param ce Create_file_log_event to process.

  @retval ERROR_STOP An error occurred - the program should terminate.
  @retval OK_CONTINUE No error, the program should continue.
*/
Exit_status  Load_log_processor::process(Create_file_log_event *ce)
{
  const char *bname= ce->fname + dirname_length(ce->fname);
  uint blen= ce->fname_len - (bname-ce->fname);

  return process_first_event(bname, blen, ce->block, ce->block_len,
                             ce->file_id, ce);
}


/**
  Process the given Begin_load_query_log_event.

  @see Load_log_processor::process_first_event(const char*,uint,const char*,uint,uint,Create_file_log_event*)

  @param ce Begin_load_query_log_event to process.

  @retval ERROR_STOP An error occurred - the program should terminate.
  @retval OK_CONTINUE No error, the program should continue.
*/
Exit_status Load_log_processor::process(Begin_load_query_log_event *blqe)
{
  return process_first_event("SQL_LOAD_MB", 11, blqe->block, blqe->block_len,
                             blqe->file_id, 0);
}


/**
  Process the given Append_block_log_event.

  Appends the chunk of the file contents specified by the event to the
  file created by a previous Begin_load_query_log_event or
  Create_file_log_event.

  If the file_id for the event does not correspond to any file
  previously registered through a Begin_load_query_log_event or
  Create_file_log_event, this member function will print a warning and
  return OK_CONTINUE.  It is safe to return OK_CONTINUE, because no
  query will be written for this event.  We should not print an error
  and fail, since the missing file_id could be because a (valid)
  --start-position has been specified after the Begin/Create event but
  before this Append event.

  @param ae Append_block_log_event to process.

  @retval ERROR_STOP An error occurred - the program should terminate.

  @retval OK_CONTINUE No error, the program should continue.
*/
Exit_status Load_log_processor::process(Append_block_log_event *ae)
{
  DBUG_ENTER("Load_log_processor::process");
  const char* fname= ((ae->file_id < file_names.elements) ?
                       dynamic_element(&file_names, ae->file_id,
                                       File_name_record*)->fname : 0);

  if (fname)
  {
    File file;
    Exit_status retval= OK_CONTINUE;
    if (((file= my_open(fname,
			O_APPEND|O_BINARY|O_WRONLY,MYF(MY_WME))) < 0))
    {
      error("Failed opening file %s", fname);
      DBUG_RETURN(ERROR_STOP);
    }
    if (my_write(file,(uchar*)ae->block,ae->block_len,MYF(MY_WME|MY_NABP)))
    {
      error("Failed writing to file %s", fname);
      retval= ERROR_STOP;
    }
    if (my_close(file,MYF(MY_WME)))
    {
      error("Failed closing file %s", fname);
      retval= ERROR_STOP;
    }
    DBUG_RETURN(retval);
  }

  /*
    There is no Create_file event (a bad binlog or a big
    --start-position). Assuming it's a big --start-position, we just do
    nothing and print a warning.
  */
  warning("Ignoring Append_block as there is no "
          "Create_file event for file_id: %u", ae->file_id);
  DBUG_RETURN(OK_CONTINUE);
}


//static Load_log_processor load_processor;


/**
  Replace windows-style backslashes by forward slashes so it can be
  consumed by the mysql client, which requires Unix path.

  @todo This is only useful under windows, so may be ifdef'ed out on
  other systems.  /Sven

  @todo If a Create_file_log_event contains a filename with a
  backslash (valid under unix), then we have problems under windows.
  /Sven

  @param[in,out] fname Filename to modify. The filename is modified
  in-place.
*/
static void convert_path_to_forward_slashes(char *fname)
{
  while (*fname)
  {
    if (*fname == '\\')
      *fname= '/';
    fname++;
  }
}


/**
  Indicates whether the given database should be filtered out,
  according to the --database=X option.

  @param log_dbname Name of database.

  @return nonzero if the database with the given name should be
  filtered out, 0 otherwise.
*/
static bool shall_skip_database(const char *log_dbname)
{
  return one_database &&
         (log_dbname != NULL) &&
         strcmp(log_dbname, database);
}


/**
  Prints the given event in base64 format.

  The header is printed to the head cache and the body is printed to
  the body cache of the print_event_info structure.  This allows all
  base64 events corresponding to the same statement to be joined into
  one BINLOG statement.

  @param[in] ev Log_event to print.
  @param[in,out] result_file FILE to which the output will be written.
  @param[in,out] print_event_info Parameters and context state
  determining how to print.

  @retval ERROR_STOP An error occurred - the program should terminate.
  @retval OK_CONTINUE No error, the program should continue.
*/
static Exit_status
write_event_header_and_base64(Log_event *ev, FILE *result_file,
                              PRINT_EVENT_INFO *print_event_info)
{
  IO_CACHE *head= &print_event_info->head_cache;
  IO_CACHE *body= &print_event_info->body_cache;
  DBUG_ENTER("write_event_header_and_base64");

  /* Write header and base64 output to cache */
  ev->print_header(head, print_event_info, FALSE);
  ev->print_base64(body, print_event_info, FALSE);

  /* Read data from cache and write to result file */
  if (copy_event_cache_to_file_and_reinit(head, result_file) ||
      copy_event_cache_to_file_and_reinit(body, result_file))
  {
    error("Error writing event to file.");
    DBUG_RETURN(ERROR_STOP);
  }
  DBUG_RETURN(OK_CONTINUE);
}

#include "tmysqlbinlogex.h"

/**
  Print the given event, and either delete it or delegate the deletion
  to someone else.

  The deletion may be delegated in two cases: (1) the event is a
  Format_description_log_event, and is saved in
  glob_description_event; (2) the event is a Create_file_log_event,
  and is saved in load_processor.

  @param[in,out] print_event_info Parameters and context state
  determining how to print.
  @param[in] ev Log_event to process.
  @param[in] pos Offset from beginning of binlog file.
  @param[in] logname Name of input binlog.

  @retval ERROR_STOP An error occurred - the program should terminate.
  @retval OK_CONTINUE No error, the program should continue.
  @retval OK_STOP No error, but the end of the specified range of
  events to process has been reached and the program should terminate.
*/
Exit_status process_event(Worker_vm* vm, Log_event *ev,
                          my_off_t pos, const char *logname)
{
  char ll_buff[21];
  Log_event_type ev_type= ev->get_type_code();
  my_bool destroy_evt= TRUE;
  DBUG_ENTER("process_event");
  Exit_status retval= OK_CONTINUE;
  PRINT_EVENT_INFO *print_event_info; 
  FILE *result_file;

  print_event_info = &vm->print_info;
  result_file = vm->result_file;

  print_event_info->short_form= short_form;

  /*
    Format events are not concerned by --offset and such, we always need to
    read them to be able to process the wanted events.
  */
  if (((rec_count >= offset) &&
       ((my_time_t)(ev->when) >= start_datetime)) ||
      (ev_type == FORMAT_DESCRIPTION_EVENT))
  {
    if (ev_type != FORMAT_DESCRIPTION_EVENT)
    {
      /*
        We have found an event after start_datetime, from now on print
        everything (in case the binlog has timestamps increasing and
        decreasing, we do this to avoid cutting the middle).
      */
      start_datetime= 0;
      offset= 0; // print everything and protect against cycling rec_count
      /*
        Skip events according to the --server-id flag.  However, don't
        skip format_description or rotate events, because they they
        are really "global" events that are relevant for the entire
        binlog, even if they have a server_id.  Also, we have to read
        the format_description event so that we can parse subsequent
        events.
      */
      if (ev_type != ROTATE_EVENT &&
          server_id && (server_id != ev->server_id))
        goto end;
    }
    if (((my_time_t)(ev->when) >= stop_datetime)
        || (pos >= stop_position_mot))
    {
      /* end the program */
      retval= OK_STOP;
      goto end;
    }
    if (!short_form)
      fprintf(result_file, "# at %s\n",llstr(pos,ll_buff));

    if (!opt_hexdump)
      print_event_info->hexdump_from= 0; /* Disabled */
    else
      print_event_info->hexdump_from= pos;

    print_event_info->base64_output_mode= opt_base64_output_mode;

    DBUG_PRINT("debug", ("event_type: %s", ev->get_type_str()));

    switch (ev_type) {
    case QUERY_EVENT:
      if (!((Query_log_event*)ev)->is_trans_keyword() &&
          shall_skip_database(((Query_log_event*)ev)->db))
        goto end;
      if (opt_base64_output_mode == BASE64_OUTPUT_ALWAYS)
      {
        if ((retval= write_event_header_and_base64(ev, result_file,
                                                   print_event_info)) !=
            OK_CONTINUE)
          goto end;
      }
      else
        ev->print(result_file, print_event_info);
      break;

    case CREATE_FILE_EVENT:
    {
      Create_file_log_event* ce= (Create_file_log_event*)ev;
      /*
        We test if this event has to be ignored. If yes, we don't save
        this event; this will have the good side-effect of ignoring all
        related Append_block and Exec_load.
        Note that Load event from 3.23 is not tested.
      */
      if (shall_skip_database(ce->db))
        goto end;                // Next event
      /*
	We print the event, but with a leading '#': this is just to inform 
	the user of the original command; the command we want to execute 
	will be a derivation of this original command (we will change the 
	filename and use LOCAL), prepared in the 'case EXEC_LOAD_EVENT' 
	below.
      */
      if (opt_base64_output_mode == BASE64_OUTPUT_ALWAYS)
      {
        if ((retval= write_event_header_and_base64(ce, result_file,
                                                   print_event_info)) !=
            OK_CONTINUE)
          goto end;
      }
      else
        ce->print(result_file, print_event_info, TRUE);

      // If this binlog is not 3.23 ; why this test??
      // glob_description_event is not safety
      if (vm->binlog_version >= 3)
      {
        /*
          transfer the responsibility for destroying the event to
          load_processor
        */
        ev= NULL;
        if ((retval= vm->load_processor.process(ce)) != OK_CONTINUE)
          goto end;
      }
      break;
    }

    case APPEND_BLOCK_EVENT:
      /*
        Append_block_log_events can safely print themselves even if
        the subsequent call load_processor.process fails, because the
        output of Append_block_log_event::print is only a comment.
      */
      ev->print(result_file, print_event_info);
      if ((retval= vm->load_processor.process((Append_block_log_event*) ev)) !=
          OK_CONTINUE)
        goto end;
      break;

    case EXEC_LOAD_EVENT:
    {
      ev->print(result_file, print_event_info);
      Execute_load_log_event *exv= (Execute_load_log_event*)ev;
      Create_file_log_event *ce= vm->load_processor.grab_event(exv->file_id);
      /*
	if ce is 0, it probably means that we have not seen the Create_file
	event (a bad binlog, or most probably --start-position is after the
	Create_file event). Print a warning comment.
      */
      if (ce)
      {
        /*
          We must not convert earlier, since the file is used by
          my_open() in Load_log_processor::append().
        */
        convert_path_to_forward_slashes((char*) ce->fname);
	ce->print(result_file, print_event_info, TRUE);
	my_free((void*)ce->fname);
	delete ce;
      }
      else
        warning("Ignoring Execute_load_log_event as there is no "
                "Create_file event for file_id: %u", exv->file_id);
      break;
    }
    case FORMAT_DESCRIPTION_EVENT:
      //glob_description_event is not safety;
      //delete glob_description_event;
      //glob_description_event= (Format_description_log_event*) ev;
      vm->binlog_version = ((Format_description_log_event*)ev)->binlog_version;
      print_event_info->common_header_len=
        ((Format_description_log_event*)ev)->common_header_len;
      ev->print(result_file, print_event_info);
      if (!remote_opt)
      {
        ev->free_temp_buf(); // free memory allocated in dump_local_log_entries
      }
      else
      {
        /*
          disassociate but not free dump_remote_log_entries time memory
        */
        ev->temp_buf= 0;
      }
      /*
        We don't want this event to be deleted now, so let's hide it (I
        (Guilhem) should later see if this triggers a non-serious Valgrind
        error). Not serious error, because we will free description_event
        later.
      */
      ev= 0;

      break;
    case BEGIN_LOAD_QUERY_EVENT:
      ev->print(result_file, print_event_info);
      if ((retval= vm->load_processor.process((Begin_load_query_log_event*) ev)) !=
          OK_CONTINUE)
        goto end;
      break;
    case EXECUTE_LOAD_QUERY_EVENT:
    {
      Execute_load_query_log_event *exlq= (Execute_load_query_log_event*)ev;
      char *fname= vm->load_processor.grab_fname(exlq->file_id);

      if (!shall_skip_database(exlq->db))
      {
        if (fname)
        {
          convert_path_to_forward_slashes(fname);
          exlq->print(result_file, print_event_info, fname);
        }
        else
          warning("Ignoring Execute_load_query since there is no "
                  "Begin_load_query event for file_id: %u", exlq->file_id);
      }

      if (fname)
	my_free(fname);
      break;
    }
    case TABLE_MAP_EVENT:
    {
      Table_map_log_event *map= ((Table_map_log_event *)ev);
      if (shall_skip_database(map->get_db_name()))
      {
        print_event_info->m_table_map_ignored.set_table(map->get_table_id(), map);
        destroy_evt= FALSE;
        goto end;
      }
    }
    case WRITE_ROWS_EVENT:
    case DELETE_ROWS_EVENT:
    case UPDATE_ROWS_EVENT:
    case PRE_GA_WRITE_ROWS_EVENT:
    case PRE_GA_DELETE_ROWS_EVENT:
    case PRE_GA_UPDATE_ROWS_EVENT:
    {
      if (ev_type != TABLE_MAP_EVENT)
      {
        Rows_log_event *e= (Rows_log_event*) ev;
        Table_map_log_event *ignored_map= 
          print_event_info->m_table_map_ignored.get_table(e->get_table_id());
        bool skip_event= (ignored_map != NULL);

        /* 
           end of statement check:
             i) destroy/free ignored maps
            ii) if skip event, flush cache now
         */
        if (e->get_flags(Rows_log_event::STMT_END_F))
        {
          /* 
            Now is safe to clear ignored map (clear_tables will also
            delete original table map events stored in the map).
          */
          if (print_event_info->m_table_map_ignored.count() > 0)
            print_event_info->m_table_map_ignored.clear_tables();

          /* 
             One needs to take into account an event that gets
             filtered but was last event in the statement. If this is
             the case, previous rows events that were written into
             IO_CACHEs still need to be copied from cache to
             result_file (as it would happen in ev->print(...) if
             event was not skipped).
          */
          if (skip_event)
          {
            if ((copy_event_cache_to_file_and_reinit(&print_event_info->head_cache, result_file) ||
                copy_event_cache_to_file_and_reinit(&print_event_info->body_cache, result_file)))
              goto err;
          }
        }

        /* skip the event check */
        if (skip_event)
          goto end;
      }
      /*
        These events must be printed in base64 format, if printed.
        base64 format requires a FD event to be safe, so if no FD
        event has been printed, we give an error.  Except if user
        passed --short-form, because --short-form disables printing
        row events.
      */
      if (!print_event_info->printed_fd_event && !short_form &&
          opt_base64_output_mode != BASE64_OUTPUT_DECODE_ROWS)
      {
        const char* type_str= ev->get_type_str();
        if (opt_base64_output_mode == BASE64_OUTPUT_NEVER)
          error("--base64-output=never specified, but binlog contains a "
                "%s event which must be printed in base64.",
                type_str);
        else
          error("malformed binlog: it does not contain any "
                "Format_description_log_event. I now found a %s event, which "
                "is not safe to process without a "
                "Format_description_log_event.",
                type_str);
        goto err;
      }
      /* FALL THROUGH */
    }
    default:
      ev->print(result_file, print_event_info);
    }
  }

  goto end;

err:
  retval= ERROR_STOP;
end:
  rec_count++;
  /*
    Destroy the log_event object. If reading from a remote host,
    set the temp_buf to NULL so that memory isn't freed twice.
  */
  if (ev)
  {
    if (remote_opt)
      ev->temp_buf= 0;
    if (destroy_evt) /* destroy it later if not set (ignored table map) */
      delete ev;
  }
  DBUG_RETURN(retval);
}


static struct my_option my_long_options[] =
{
  {"help", '?', "Display this help and exit.",
   0, 0, 0, GET_NO_ARG, NO_ARG, 0, 0, 0, 0, 0, 0},
  {"base64-output", OPT_BASE64_OUTPUT_MODE,
    /* 'unspec' is not mentioned because it is just a placeholder. */
   "Determine when the output statements should be base64-encoded BINLOG "
   "statements: 'never' disables it and works only for binlogs without "
   "row-based events; 'decode-rows' decodes row events into commented SQL "
   "statements if the --verbose option is also given; 'auto' prints base64 "
   "only when necessary (i.e., for row-based events and format description "
   "events); 'always' prints base64 whenever possible. 'always' is "
   "deprecated, will be removed in a future version, and should not be used "
   "in a production system.  --base64-output with no 'name' argument is "
   "equivalent to --base64-output=always and is also deprecated.  If no "
   "--base64-output[=name] option is given at all, the default is 'auto'.",
   &opt_base64_output_mode_str, &opt_base64_output_mode_str,
   0, GET_STR, OPT_ARG, 0, 0, 0, 0, 0, 0},
  /*
    mysqlbinlog needs charsets knowledge, to be able to convert a charset
    number found in binlog to a charset name (to be able to print things
    like this:
    SET @`a`:=_cp850 0x4DFC6C6C6572 COLLATE `cp850_general_ci`;
  */
  {"character-sets-dir", OPT_CHARSETS_DIR,
   "Directory for character set files.", &charsets_dir,
   &charsets_dir, 0, GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {"concurrency", 'c', 
   "Concurrency of binlog executed(default 4). ", &concurrency, 
   &concurrency, 0, GET_UINT, REQUIRED_ARG, 4, 1, 64, 0, 0, 0}, 
  {"database", 'd', "List entries for just this database (local log only).",
   &database, &database, 0, GET_STR_ALLOC, REQUIRED_ARG,
   0, 0, 0, 0, 0, 0},
#ifndef DBUG_OFF
  {"debug", '#', "Output debug log.", &default_dbug_option,
   &default_dbug_option, 0, GET_STR, OPT_ARG, 0, 0, 0, 0, 0, 0},
#endif
  {"debug-check", OPT_DEBUG_CHECK, "Check memory and open file usage at exit .",
   &debug_check_flag, &debug_check_flag, 0,
   GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0},
  {"debug-info", OPT_DEBUG_INFO, "Print some debug info at exit.",
   &debug_info_flag, &debug_info_flag,
   0, GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0},
  {"default_auth", OPT_DEFAULT_AUTH,
   "Default authentication client-side plugin to use.",
   &opt_default_auth, &opt_default_auth, 0,
   GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {"disable-log-bin", 'D', "Disable binary log. This is useful, if you "
    "enabled --to-last-log and are sending the output to the same MySQL server. "
    "This way you could avoid an endless loop. You would also like to use it "
    "when restoring after a crash to avoid duplication of the statements you "
    "already have. NOTE: you will need a SUPER privilege to use this option.",
   &disable_log_bin, &disable_log_bin, 0, GET_BOOL,
   NO_ARG, 0, 0, 0, 0, 0, 0},
  {"exit-when-error", OPT_EXIT_WHEN_ERROR, "Exit when error occured.",
   &exit_when_error, &exit_when_error, 0, GET_BOOL, NO_ARG,
   0, 0, 0, 0, 0, 0},
  {"force-if-open", 'F', "Force if binlog was not closed properly.",
   &force_if_open_opt, &force_if_open_opt, 0, GET_BOOL, NO_ARG,
   1, 0, 0, 0, 0, 0},
  {"force-read", 'f', "Force reading unknown binlog events.",
   &force_opt, &force_opt, 0, GET_BOOL, NO_ARG, 0, 0, 0, 0,
   0, 0},
  {"hexdump", 'H', "Augment output with hexadecimal and ASCII event dump.",
   &opt_hexdump, &opt_hexdump, 0, GET_BOOL, NO_ARG,
   0, 0, 0, 0, 0, 0},
  /*{"host", 'h', "Get the binlog from server.", &host, &host,
   0, GET_STR_ALLOC, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},*/
  {"local-load", 'l', "Prepare local temporary files for LOAD DATA INFILE in the specified directory.",
   &dirname_for_local_load, &dirname_for_local_load, 0,
   GET_STR_ALLOC, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {"offset", 'o', "Skip the first N entries.", &offset, &offset,
   0, GET_ULL, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  /*{"password", 'p', "Password to connect to remote server.",
   0, 0, 0, GET_STR, OPT_ARG, 0, 0, 0, 0, 0, 0},*/
  {"plugin_dir", OPT_PLUGIN_DIR, "Directory for client-side plugins.",
    &opt_plugin_dir, &opt_plugin_dir, 0,
   GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  /*{"port", 'P', "Port number to use for connection or 0 for default to, in "
   "order of preference, my.cnf, $MYSQL_TCP_PORT, "
#if MYSQL_PORT_DEFAULT == 0
   "/etc/services, "
#endif
   "built-in default (" STRINGIFY_ARG(MYSQL_PORT) ").",
   &port, &port, 0, GET_INT, REQUIRED_ARG,
   0, 0, 0, 0, 0, 0}, */
  {"protocol", OPT_MYSQL_PROTOCOL,
   "The protocol to use for connection (tcp, socket, pipe, memory).",
   0, 0, 0, GET_STR,  REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  /*{"read-from-remote-server", 'R', "Read binary logs from a MySQL server.",
   &remote_opt, &remote_opt, 0, GET_BOOL, NO_ARG, 0, 0, 0, 0,
   0, 0},*/
  {"remote-host", OPT_REMOTE_HOST, "Binlog would be executed in server.", &remote_host, &remote_host,
   0, GET_STR_ALLOC, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {"remote-password", OPT_REMOTE_PASS, "Password to connect to the remote-host as remote-user.", 0, 0,
   0, GET_STR, OPT_ARG, 0, 0, 0, 0, 0, 0},
  {"remote-port", OPT_REMOTE_PORT, "Port number to remote-user for connection to the remote-host.", &remote_port, &remote_port,
   0, GET_INT, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {"remote-user", OPT_REMOTE_USER, "Connect to the remote-host as username.", &remote_user, &remote_user,
   0, GET_STR_ALLOC, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {"result-file", 'r', "Direct output to a given file.", 0, 0, 0, GET_STR,
   REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {"server-id", OPT_SERVER_ID,
   "Extract only binlog entries created by the server having the given id.",
   &server_id, &server_id, 0, GET_ULONG,
   REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {"set-charset", OPT_SET_CHARSET,
   "Add 'SET NAMES character_set' to the output.", &charset,
   &charset, 0, GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
#ifdef HAVE_SMEM
  {"shared-memory-base-name", OPT_SHARED_MEMORY_BASE_NAME,
   "Base name of shared memory.", &shared_memory_base_name,
   &shared_memory_base_name,
   0, GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
#endif
  {"short-form", 's', "Just show regular queries: no extra info and no "
   "row-based events. This is for testing only, and should not be used in "
   "production systems. If you want to suppress base64-output, consider "
   "using --base64-output=never instead.",
   &short_form, &short_form, 0, GET_BOOL, NO_ARG, 0, 0, 0, 0,
   0, 0},
  /*{"socket", 'S', "The socket file to use for connection.",
   &sock, &sock, 0, GET_STR, REQUIRED_ARG, 0, 0, 0, 0,
   0, 0},*/
  {"sql-files-output-dir", OPT_SQL_OUTPUT_DIR, "Binlog sql files saved in this directory, named thread_<thread_id>.sql .",
  &sql_files_output_dir, &sql_files_output_dir,
  0, GET_STR_ALLOC, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {"start-datetime", OPT_START_DATETIME,
   "Start reading the binlog at first event having a datetime equal or "
   "posterior to the argument; the argument must be a date and time "
   "in the local time zone, in any format accepted by the MySQL server "
   "for DATETIME and TIMESTAMP types, for example: 2004-12-25 11:25:56 "
   "(you should probably use quotes for your shell to set it properly).",
   &start_datetime_str, &start_datetime_str,
   0, GET_STR_ALLOC, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {"start-position", 'j',
   "Start reading the binlog at position N. Applies to the first binlog "
   "passed on the command line.",
   &start_position, &start_position, 0, GET_ULL,
   REQUIRED_ARG, BIN_LOG_HEADER_SIZE, BIN_LOG_HEADER_SIZE,
   /* COM_BINLOG_DUMP accepts only 4 bytes for the position */
   (ulonglong)(~(uint32)0), 0, 0, 0},
  {"stop-datetime", OPT_STOP_DATETIME,
   "Stop reading the binlog at first event having a datetime equal or "
   "posterior to the argument; the argument must be a date and time "
   "in the local time zone, in any format accepted by the MySQL server "
   "for DATETIME and TIMESTAMP types, for example: 2004-12-25 11:25:56 "
   "(you should probably use quotes for your shell to set it properly).",
   &stop_datetime_str, &stop_datetime_str,
   0, GET_STR_ALLOC, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {"stop-position", OPT_STOP_POSITION,
   "Stop reading the binlog at position N. Applies to the last binlog "
   "passed on the command line.",
   &stop_position, &stop_position, 0, GET_ULL,
   REQUIRED_ARG, (ulonglong)(~(my_off_t)0), BIN_LOG_HEADER_SIZE,
   (ulonglong)(~(my_off_t)0), 0, 0, 0},
  {"to-last-log", 't', "Requires -R. Will not stop at the end of the \
requested binlog but rather continue printing until the end of the last \
binlog of the MySQL server. If you send the output to the same MySQL server, \
that may lead to an endless loop.",
   &to_last_remote_log, &to_last_remote_log, 0, GET_BOOL,
   NO_ARG, 0, 0, 0, 0, 0, 0},
  {"table-split", OPT_TABLE_SPLIT, "Rule of table spliting. ", &table_split, &table_split,
   0, GET_STR_ALLOC, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  /*{"user", 'u', "Connect to the remote server as username.",
   &user, &user, 0, GET_STR_ALLOC, REQUIRED_ARG, 0, 0, 0, 0,
   0, 0},*/
  {"verbose", 'v', "Reconstruct SQL statements out of row events. "
                   "-v -v adds comments on column data types.",
   0, 0, 0, GET_NO_ARG, NO_ARG, 0, 0, 0, 0, 0, 0},
  {"version", 'V', "Print version and exit.", 0, 0, 0, GET_NO_ARG, NO_ARG, 0,
   0, 0, 0, 0, 0},
  {"open_files_limit", OPT_OPEN_FILES_LIMIT,
   "Used to reserve file descriptors for use by this program.",
   &open_files_limit, &open_files_limit, 0, GET_ULONG,
   REQUIRED_ARG, MY_NFILE, 8, OS_FILE_LIMIT, 0, 1, 0},
  {0, 0, 0, 0, 0, 0, GET_NO_ARG, NO_ARG, 0, 0, 0, 0, 0, 0},
  {"with-mysql", OPT_WITH_MYSQL, "mysql path",
   &mysql_path, &mysql_path, 0,
   GET_STR_ALLOC, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {"write-to-file-only", OPT_WRITE_TO_FILE_ONLY, "Only write to file in --sql-files-output-dir, but don't execute. ",
   &write_to_file_only_flag, &write_to_file_only_flag, 0, GET_BOOL, NO_ARG, 0, 0, 0, 0,
   0, 0},
};


/**
  Auxiliary function used by error() and warning().

  Prints the given text (normally "WARNING: " or "ERROR: "), followed
  by the given vprintf-style string, followed by a newline.

  @param format Printf-style format string.
  @param args List of arguments for the format string.
  @param msg Text to print before the string.
*/
static void error_or_warning(const char *format, va_list args, const char *msg)
{
  fprintf(stderr, "%s: ", msg);
  vfprintf(stderr, format, args);
  fprintf(stderr, "\n");
}

/**
  Prints a message to stderr, prefixed with the text "ERROR: " and
  suffixed with a newline.

  @param format Printf-style format string, followed by printf
  varargs.
*/
static void error(const char *format,...)
{
  va_list args;
  va_start(args, format);
  error_or_warning(format, args, "ERROR");
  va_end(args);
}


/**
  This function is used in log_event.cc to report errors.

  @param format Printf-style format string, followed by printf
  varargs.
*/
static void sql_print_error(const char *format,...)
{
  va_list args;
  va_start(args, format);
  error_or_warning(format, args, "ERROR");
  va_end(args);
}

/**
  Prints a message to stderr, prefixed with the text "WARNING: " and
  suffixed with a newline.

  @param format Printf-style format string, followed by printf
  varargs.
*/
static void warning(const char *format,...)
{
  va_list args;
  va_start(args, format);
  error_or_warning(format, args, "WARNING");
  va_end(args);
}

/**
  Frees memory for global variables in this file.
*/
static void cleanup()
{
  my_free(pass);
  my_free(database);
  my_free(host);
  my_free(user);
  my_free(dirname_for_local_load);

  delete glob_description_event;
  if (mysql)
    mysql_close(mysql);
}


static void print_version()
{
  printf("%s Ver 3.3 for %s at %s\n", my_progname, SYSTEM_TYPE, MACHINE_TYPE);
}


static void usage()
{
  print_version();
  puts(ORACLE_WELCOME_COPYRIGHT_NOTICE("2000, 2011"));
  printf("\
Dumps a MySQL binary log in a format usable for viewing or for piping to\n\
the mysql command line client.\n\n");
  printf("Usage: %s [options] log-files\n", my_progname);
  my_print_help(my_long_options);
  my_print_variables(my_long_options);
}


static my_time_t convert_str_to_timestamp(const char* str)
{
  int was_cut;
  MYSQL_TIME l_time;
  long dummy_my_timezone;
  my_bool dummy_in_dst_time_gap;
  /* We require a total specification (date AND time) */
  if (str_to_datetime(str, (uint) strlen(str), &l_time, 0, &was_cut) !=
      MYSQL_TIMESTAMP_DATETIME || was_cut)
  {
    error("Incorrect date and time argument: %s", str);
    exit(1);
  }
  /*
    Note that Feb 30th, Apr 31st cause no error messages and are mapped to
    the next existing day, like in mysqld. Maybe this could be changed when
    mysqld is changed too (with its "strict" mode?).
  */
  return
    my_system_gmt_sec(&l_time, &dummy_my_timezone, &dummy_in_dst_time_gap);
}


extern "C" my_bool
get_one_option(int optid, const struct my_option *opt __attribute__((unused)),
	       char *argument)
{
  bool tty_password=0;
  switch (optid) {
#ifndef DBUG_OFF
  case '#':
    DBUG_PUSH(argument ? argument : default_dbug_option);
    break;
#endif
  case 'd':
    one_database = 1;
    break;
  case 'p':
    if (argument == disabled_my_option)
      argument= (char*) "";                     // Don't require password
    if (argument)
    {
      my_free(pass);
      char *start=argument;
      pass= my_strdup(argument,MYF(MY_FAE));
      while (*argument) *argument++= 'x';		/* Destroy argument */
      if (*start)
        start[1]=0;				/* Cut length of argument */
    }
    else
      tty_password=1;
    break;
  case OPT_REMOTE_PASS:
      if (argument == disabled_my_option)
          argument= (char*) "";                     // Don't require password
      if (argument)
      {
          my_free(remote_pass);
          char *start=argument;
          remote_pass = my_strdup(argument,MYF(MY_FAE));
          while (*argument) *argument++= 'x';		/* Destroy argument */
          if (*start)
              start[1]=0;				/* Cut length of argument */
      }
      else
          remote_pass = get_tty_password(NullS);
          //tty_password=1;
      break;
  //case 'r':
  //  if (!(result_file = my_fopen(argument, O_WRONLY | O_BINARY, MYF(MY_WME))))
  //    exit(1);
  //  break;
  case 'R':
    remote_opt= 1;
    break;
  case OPT_MYSQL_PROTOCOL:
    opt_protocol= find_type_or_exit(argument, &sql_protocol_typelib,
                                    opt->name);
    break;
  case OPT_START_DATETIME:
    start_datetime= convert_str_to_timestamp(start_datetime_str);
    break;
  case OPT_STOP_DATETIME:
    stop_datetime= convert_str_to_timestamp(stop_datetime_str);
    break;
  case OPT_BASE64_OUTPUT_MODE:
    if (argument == NULL)
      opt_base64_output_mode= BASE64_OUTPUT_ALWAYS;
    else
    {
      opt_base64_output_mode= (enum_base64_output_mode)
        (find_type_or_exit(argument, &base64_output_mode_typelib, opt->name)-1);
    }
    break;
  case 'v':
    if (argument == disabled_my_option)
      verbose= 0;
    else
      verbose++;
    break;
  case 'V':
    print_version();
    exit(0);
  case '?':
    usage();
    exit(0);
  }
  if (tty_password)
    pass= get_tty_password(NullS);

  return 0;
}


static int parse_args(int *argc, char*** argv)
{
  int ho_error;

  //result_file = stdout;
  if ((ho_error=handle_options(argc, argv, my_long_options, get_one_option)))
    exit(ho_error);
  if (debug_info_flag)
    my_end_arg= MY_CHECK_ERROR | MY_GIVE_INFO;
  if (debug_check_flag)
    my_end_arg= MY_CHECK_ERROR;
  return 0;
}


/**
  Create and initialize the global mysql object, and connect to the
  server.

  @retval ERROR_STOP An error occurred - the program should terminate.
  @retval OK_CONTINUE No error, the program should continue.
*/
static Exit_status safe_connect()
{
  mysql= mysql_init(NULL);

  if (!mysql)
  {
    error("Failed on mysql_init.");
    return ERROR_STOP;
  }

  if (opt_plugin_dir && *opt_plugin_dir)
    mysql_options(mysql, MYSQL_PLUGIN_DIR, opt_plugin_dir);

  if (opt_default_auth && *opt_default_auth)
    mysql_options(mysql, MYSQL_DEFAULT_AUTH, opt_default_auth);

  if (opt_protocol)
    mysql_options(mysql, MYSQL_OPT_PROTOCOL, (char*) &opt_protocol);
#ifdef HAVE_SMEM
  if (shared_memory_base_name)
    mysql_options(mysql, MYSQL_SHARED_MEMORY_BASE_NAME,
                  shared_memory_base_name);
#endif
  if (!mysql_real_connect(mysql, host, user, pass, 0, port, sock, 0))
  {
    error("Failed on connect: %s", mysql_error(mysql));
    return ERROR_STOP;
  }
  mysql->reconnect= 1;
  return OK_CONTINUE;
}


/**
  High-level function for dumping a named binlog.

  This function calls dump_remote_log_entries() or
  dump_local_log_entries() to do the job.

  @param[in] logname Name of input binlog.

  @retval ERROR_STOP An error occurred - the program should terminate.
  @retval OK_CONTINUE No error, the program should continue.
  @retval OK_STOP No error, but the end of the specified range of
  events to process has been reached and the program should terminate.
*/
static Exit_status dump_log_entries(const char* logname)
{
  Exit_status rc;
  PRINT_EVENT_INFO print_event_info;

  if (!print_event_info.init_ok())
    return ERROR_STOP;
  /*
     Set safe delimiter, to dump things
     like CREATE PROCEDURE safely
  */
  //fprintf(result_file, "DELIMITER /*!*/;\n");
  strmov(print_event_info.delimiter, "/*!*/;");
  
  print_event_info.verbose= short_form ? 0 : verbose;

  //TODO : support remote
  //rc= (remote_opt ? dump_remote_log_entries(&print_event_info, logname) :
  rc =dump_local_log_entries(&print_event_info, logname);

  /* Set delimiter back to semicolon */
  //fprintf(result_file, "DELIMITER ;\n");
  strmov(print_event_info.delimiter, ";");
  return rc;
}


/**
  When reading a remote binlog, this function is used to grab the
  Format_description_log_event in the beginning of the stream.
  
  This is not as smart as check_header() (used for local log); it will
  not work for a binlog which mixes format. TODO: fix this.

  @retval ERROR_STOP An error occurred - the program should terminate.
  @retval OK_CONTINUE No error, the program should continue.
*/
static Exit_status check_master_version()
{
  MYSQL_RES* res = 0;
  MYSQL_ROW row;
  const char* version;

  if (mysql_query(mysql, "SELECT VERSION()") ||
      !(res = mysql_store_result(mysql)))
  {
    error("Could not find server version: "
          "Query failed when checking master version: %s", mysql_error(mysql));
    return ERROR_STOP;
  }
  if (!(row = mysql_fetch_row(res)))
  {
    error("Could not find server version: "
          "Master returned no rows for SELECT VERSION().");
    goto err;
  }

  if (!(version = row[0]))
  {
    error("Could not find server version: "
          "Master reported NULL for the version.");
    goto err;
  }

  delete glob_description_event;
  switch (*version) {
  case '3':
    glob_description_event= new Format_description_log_event(1);
    break;
  case '4':
    glob_description_event= new Format_description_log_event(3);
    break;
  case '5':
    /*
      The server is soon going to send us its Format_description log
      event, unless it is a 5.0 server with 3.23 or 4.0 binlogs.
      So we first assume that this is 4.0 (which is enough to read the
      Format_desc event if one comes).
    */
    glob_description_event= new Format_description_log_event(3);
    break;
  default:
    glob_description_event= NULL;
    error("Could not find server version: "
          "Master reported unrecognized MySQL version '%s'.", version);
    goto err;
  }
  if (!glob_description_event || !glob_description_event->is_valid())
  {
    error("Failed creating Format_description_log_event; out of memory?");
    goto err;
  }

  mysql_free_result(res);
  return OK_CONTINUE;

err:
  mysql_free_result(res);
  return ERROR_STOP;
}


/**
  Requests binlog dump from a remote server and prints the events it
  receives.

  @param[in,out] print_event_info Parameters and context state
  determining how to print.
  @param[in] logname Name of input binlog.

  @retval ERROR_STOP An error occurred - the program should terminate.
  @retval OK_CONTINUE No error, the program should continue.
  @retval OK_STOP No error, but the end of the specified range of
  events to process has been reached and the program should terminate.
*/
//static Exit_status dump_remote_log_entries(PRINT_EVENT_INFO *print_event_info,
//                                           const char* logname)
//
//{
//  uchar buf[128];
//  ulong len;
//  uint logname_len;
//  NET* net;
//  my_off_t old_off= start_position_mot;
//  char fname[FN_REFLEN+1];
//  Exit_status retval= OK_CONTINUE;
//  DBUG_ENTER("dump_remote_log_entries");
//
//  /*
//    Even if we already read one binlog (case of >=2 binlogs on command line),
//    we cannot re-use the same connection as before, because it is now dead
//    (COM_BINLOG_DUMP kills the thread when it finishes).
//  */
//  if ((retval= safe_connect()) != OK_CONTINUE)
//    DBUG_RETURN(retval);
//  net= &mysql->net;
//
//  if ((retval= check_master_version()) != OK_CONTINUE)
//    DBUG_RETURN(retval);
//
//  /*
//    COM_BINLOG_DUMP accepts only 4 bytes for the position, so we are forced to
//    cast to uint32.
//  */
//  int4store(buf, (uint32)start_position);
//  int2store(buf + BIN_LOG_HEADER_SIZE, binlog_flags);
//
//  size_t tlen = strlen(logname);
//  if (tlen > UINT_MAX) 
//  {
//    error("Log name too long.");
//    DBUG_RETURN(ERROR_STOP);
//  }
//  logname_len = (uint) tlen;
//  int4store(buf + 6, 0);
//  memcpy(buf + 10, logname, logname_len);
//  if (simple_command(mysql, COM_BINLOG_DUMP, buf, logname_len + 10, 1))
//  {
//    error("Got fatal error sending the log dump command.");
//    DBUG_RETURN(ERROR_STOP);
//  }
//
//  for (;;)
//  {
//    const char *error_msg;
//    Log_event *ev;
//
//    len= cli_safe_read(mysql);
//    if (len == packet_error)
//    {
//      error("Got error reading packet from server: %s", mysql_error(mysql));
//      DBUG_RETURN(ERROR_STOP);
//    }
//    if (len < 8 && net->read_pos[0] == 254)
//      break; // end of data
//    DBUG_PRINT("info",( "len: %lu  net->read_pos[5]: %d\n",
//			len, net->read_pos[5]));
//    if (!(ev= Log_event::read_log_event((const char*) net->read_pos + 1 ,
//                                        len - 1, &error_msg,
//                                        glob_description_event)))
//    {
//      error("Could not construct log event object: %s", error_msg);
//      DBUG_RETURN(ERROR_STOP);
//    }   
//    /*
//      If reading from a remote host, ensure the temp_buf for the
//      Log_event class is pointing to the incoming stream.
//    */
//    ev->register_temp_buf((char *) net->read_pos + 1);
//
//    Log_event_type type= ev->get_type_code();
//    if (glob_description_event->binlog_version >= 3 ||
//        (type != LOAD_EVENT && type != CREATE_FILE_EVENT))
//    {
//      /*
//        If this is a Rotate event, maybe it's the end of the requested binlog;
//        in this case we are done (stop transfer).
//        This is suitable for binlogs, not relay logs (but for now we don't read
//        relay logs remotely because the server is not able to do that). If one
//        day we read relay logs remotely, then we will have a problem with the
//        detection below: relay logs contain Rotate events which are about the
//        binlogs, so which would trigger the end-detection below.
//      */
//      if (type == ROTATE_EVENT)
//      {
//        Rotate_log_event *rev= (Rotate_log_event *)ev;
//        /*
//          If this is a fake Rotate event, and not about our log, we can stop
//          transfer. If this a real Rotate event (so it's not about our log,
//          it's in our log describing the next log), we print it (because it's
//          part of our log) and then we will stop when we receive the fake one
//          soon.
//        */
//        if (rev->when == 0)
//        {
//          if (!to_last_remote_log)
//          {
//            if ((rev->ident_len != logname_len) ||
//                memcmp(rev->new_log_ident, logname, logname_len))
//            {
//              DBUG_RETURN(OK_CONTINUE);
//            }
//            /*
//              Otherwise, this is a fake Rotate for our log, at the very
//              beginning for sure. Skip it, because it was not in the original
//              log. If we are running with to_last_remote_log, we print it,
//              because it serves as a useful marker between binlogs then.
//            */
//            continue;
//          }
//          len= 1; // fake Rotate, so don't increment old_off
//        }
//      }
//      else if (type == FORMAT_DESCRIPTION_EVENT)
//      {
//        /*
//          This could be an fake Format_description_log_event that server
//          (5.0+) automatically sends to a slave on connect, before sending
//          a first event at the requested position.  If this is the case,
//          don't increment old_off. Real Format_description_log_event always
//          starts from BIN_LOG_HEADER_SIZE position.
//        */
//        if (old_off != BIN_LOG_HEADER_SIZE)
//          len= 1;         // fake event, don't increment old_off
//      }
//      Exit_status retval= process_event(print_event_info, ev, old_off, logname);
//      if (retval != OK_CONTINUE)
//        DBUG_RETURN(retval);
//    }
//    else
//    {
//      Load_log_event *le= (Load_log_event*)ev;
//      const char *old_fname= le->fname;
//      uint old_len= le->fname_len;
//      File file;
//      Exit_status retval;
//
//      if ((file= load_processor.prepare_new_file_for_old_format(le,fname)) < 0)
//        DBUG_RETURN(ERROR_STOP);
//
//      retval= process_event(print_event_info, ev, old_off, logname);
//      if (retval != OK_CONTINUE)
//      {
//        my_close(file,MYF(MY_WME));
//        DBUG_RETURN(retval);
//      }
//      retval= load_processor.load_old_format_file(net,old_fname,old_len,file);
//      my_close(file,MYF(MY_WME));
//      if (retval != OK_CONTINUE)
//        DBUG_RETURN(retval);
//    }
//    /*
//      Let's adjust offset for remote log as for local log to produce
//      similar text and to have --stop-position to work identically.
//    */
//    old_off+= len-1;
//  }
//
//  DBUG_RETURN(OK_CONTINUE);
//}
//

/**
  Reads the @c Format_description_log_event from the beginning of a
  local input file.

  The @c Format_description_log_event is only read if it is outside
  the range specified with @c --start-position; otherwise, it will be
  seen later.  If this is an old binlog, a fake @c
  Format_description_event is created.  This also prints a @c
  Format_description_log_event to the output, unless we reach the
  --start-position range.  In this case, it is assumed that a @c
  Format_description_log_event will be found when reading events the
  usual way.

  @param file The file to which a @c Format_description_log_event will
  be printed.

  @param[in,out] print_event_info Parameters and context state
  determining how to print.

  @param[in] logname Name of input binlog.

  @retval ERROR_STOP An error occurred - the program should terminate.
  @retval OK_CONTINUE No error, the program should continue.
  @retval OK_STOP No error, but the end of the specified range of
  events to process has been reached and the program should terminate.
*/
static Exit_status check_header(IO_CACHE* file,
                                PRINT_EVENT_INFO *print_event_info,
                                const char* logname)
{
  uchar header[BIN_LOG_HEADER_SIZE];
  uchar buf[PROBE_HEADER_LEN];
  my_off_t tmp_pos, pos;

  delete glob_description_event;
  if (!(glob_description_event= new Format_description_log_event(3)))
  {
    error("Failed creating Format_description_log_event; out of memory?");
    return ERROR_STOP;
  }

  pos= my_b_tell(file);
  my_b_seek(file, (my_off_t)0);
  if (my_b_read(file, header, sizeof(header)))
  {
    error("Failed reading header; probably an empty file.");
    return ERROR_STOP;
  }
  if (memcmp(header, BINLOG_MAGIC, sizeof(header)))
  {
    error("File is not a binary log file.");
    return ERROR_STOP;
  }

  /*
    Imagine we are running with --start-position=1000. We still need
    to know the binlog format's. So we still need to find, if there is
    one, the Format_desc event, or to know if this is a 3.23
    binlog. So we need to first read the first events of the log,
    those around offset 4.  Even if we are reading a 3.23 binlog from
    the start (no --start-position): we need to know the header length
    (which is 13 in 3.23, 19 in 4.x) to be able to successfully print
    the first event (Start_log_event_v3). So even in this case, we
    need to "probe" the first bytes of the log *before* we do a real
    read_log_event(). Because read_log_event() needs to know the
    header's length to work fine.
  */
  for(;;)
  {
    tmp_pos= my_b_tell(file); /* should be 4 the first time */
    if (my_b_read(file, buf, sizeof(buf)))
    {
      if (file->error)
      {
        error("Could not read entry at offset %llu: "
              "Error in log format or read error.", (ulonglong)tmp_pos);
        return ERROR_STOP;
      }
      /*
        Otherwise this is just EOF : this log currently contains 0-2
        events.  Maybe it's going to be filled in the next
        milliseconds; then we are going to have a problem if this a
        3.23 log (imagine we are locally reading a 3.23 binlog which
        is being written presently): we won't know it in
        read_log_event() and will fail().  Similar problems could
        happen with hot relay logs if --start-position is used (but a
        --start-position which is posterior to the current size of the log).
        These are rare problems anyway (reading a hot log + when we
        read the first events there are not all there yet + when we
        read a bit later there are more events + using a strange
        --start-position).
      */
      break;
    }
    else
    {
      DBUG_PRINT("info",("buf[EVENT_TYPE_OFFSET=%d]=%d",
                         EVENT_TYPE_OFFSET, buf[EVENT_TYPE_OFFSET]));
      /* always test for a Start_v3, even if no --start-position */
      if (buf[EVENT_TYPE_OFFSET] == START_EVENT_V3)
      {
        /* This is 3.23 or 4.x */
        if (uint4korr(buf + EVENT_LEN_OFFSET) < 
            (LOG_EVENT_MINIMAL_HEADER_LEN + START_V3_HEADER_LEN))
        {
          /* This is 3.23 (format 1) */
          delete glob_description_event;
          if (!(glob_description_event= new Format_description_log_event(1)))
          {
            error("Failed creating Format_description_log_event; "
                  "out of memory?");
            return ERROR_STOP;
          }
        }
        break;
      }
      else if (tmp_pos >= start_position)
        break;
      /* �����������¼� */
      else if (buf[EVENT_TYPE_OFFSET] == FORMAT_DESCRIPTION_EVENT)
      {
        /* This is 5.0 */
        Format_description_log_event *new_description_event;
        my_b_seek(file, tmp_pos); /* seek back to event's start */
        if (!(new_description_event= (Format_description_log_event*) 
              Log_event::read_log_event(file, glob_description_event)))
          /* EOF can't be hit here normally, so it's a real error */
        {
          error("Could not read a Format_description_log_event event at "
                "offset %llu; this could be a log format error or read error.",
                (ulonglong)tmp_pos);
          return ERROR_STOP;
        }
        if (opt_base64_output_mode == BASE64_OUTPUT_AUTO
            || opt_base64_output_mode == BASE64_OUTPUT_ALWAYS)
        {
          /*
            process_event will delete *description_event and set it to
            the new one, so we should not do it ourselves in this
            case.
          */
          Exit_status retval= binlogex_process_event(new_description_event, tmp_pos,
                                            logname);
          if (retval != OK_CONTINUE)
            return retval;
        }
        else
        {
          delete glob_description_event;
          glob_description_event= new_description_event;
        }
        DBUG_PRINT("info",("Setting description_event"));
      }
      else if (buf[EVENT_TYPE_OFFSET] == ROTATE_EVENT)
      {
        Log_event *ev;
        my_b_seek(file, tmp_pos); /* seek back to event's start */
        if (!(ev= Log_event::read_log_event(file, glob_description_event)))
        {
          /* EOF can't be hit here normally, so it's a real error */
          error("Could not read a Rotate_log_event event at offset %llu;"
                " this could be a log format error or read error.",
                (ulonglong)tmp_pos);
          return ERROR_STOP;
        }
        delete ev;
      }
      else
        break;
    }
  }
  my_b_seek(file, pos);
  return OK_CONTINUE;
}


/**
  Reads a local binlog and prints the events it sees.

  @param[in] logname Name of input binlog.

  @param[in,out] print_event_info Parameters and context state
  determining how to print.

  @retval ERROR_STOP An error occurred - the program should terminate.
  @retval OK_CONTINUE No error, the program should continue.
  @retval OK_STOP No error, but the end of the specified range of
  events to process has been reached and the program should terminate.
*/
static Exit_status dump_local_log_entries(PRINT_EVENT_INFO *print_event_info,
                                          const char* logname)
{
    File fd = -1;
    IO_CACHE cache,*file= &cache;
    uchar tmp_buff[BIN_LOG_HEADER_SIZE];
    Exit_status retval= OK_CONTINUE;

    if (logname && strcmp(logname, "-") != 0)
    {
        /* read from normal file */
        if ((fd = my_open(logname, O_RDONLY | O_BINARY, MYF(MY_WME))) < 0)
            return ERROR_STOP;
        if (init_io_cache(file, fd, 0, READ_CACHE, start_position_mot, 0,
            MYF(MY_WME | MY_NABP)))
        {
            my_close(fd, MYF(MY_WME));
            return ERROR_STOP;
        }
        if ((retval= check_header(file, print_event_info, logname)) != OK_CONTINUE)
            goto end;
    }
    else
    {
        /* read from stdin */
        /*
        Windows opens stdin in text mode by default. Certain characters
        such as CTRL-Z are interpeted as events and the read() method
        will stop. CTRL-Z is the EOF marker in Windows. to get past this
        you have to open stdin in binary mode. Setmode() is used to set
        stdin in binary mode. Errors on setting this mode result in 
        halting the function and printing an error message to stderr.
        */
#if defined (__WIN__) || (_WIN64)
        if (_setmode(fileno(stdin), O_BINARY) == -1)
        {
            error("Could not set binary mode on stdin.");
            return ERROR_STOP;
        }
#endif 
        if (init_io_cache(file, my_fileno(stdin), 0, READ_CACHE, (my_off_t) 0,
            0, MYF(MY_WME | MY_NABP | MY_DONT_CHECK_FILESIZE)))
        {
            error("Failed to init IO cache.");
            return ERROR_STOP;
        }
        if ((retval= check_header(file, print_event_info, logname)) != OK_CONTINUE)
            goto end;
        if (start_position)
        {
            /* skip 'start_position' characters from stdin */
            uchar buff[IO_SIZE];
            my_off_t length,tmp;
            for (length= start_position_mot ; length > 0 ; length-=tmp)
            {
                tmp=min(length,sizeof(buff));
                if (my_b_read(file, buff, (uint) tmp))
                {
                    error("Failed reading from file.");
                    goto err;
                }
            }
        }
    }

    if (!glob_description_event || !glob_description_event->is_valid())
    {
        error("Invalid Format_description log event; could be out of memory.");
        goto err;
    }

    if (!start_position && my_b_read(file, tmp_buff, BIN_LOG_HEADER_SIZE))
    {
        error("Failed reading from file.");
        goto err;
    }
    for (;;)
    {
        char llbuff[21];
        my_off_t old_off = my_b_tell(file);

        Log_event* ev = Log_event::read_log_event(file, glob_description_event);
        if (!ev)
        {
            /*
            if binlog wasn't closed properly ("in use" flag is set) don't complain
            about a corruption, but treat it as EOF and move to the next binlog.
            */
            if (glob_description_event->flags & LOG_EVENT_BINLOG_IN_USE_F)
                file->error= 0;
            else if (file->error)
            {
                error("Could not read entry at offset %s: "
                    "Error in log format or read error.",
                    llstr(old_off,llbuff));
                goto err;
            }
            // file->error == 0 means EOF, that's OK, we break in this case
            goto end;
        }
        if ((retval= binlogex_process_event(ev, old_off, logname)) !=
            OK_CONTINUE)
            goto end;
    }

    /* NOTREACHED */

err:
    retval= ERROR_STOP;

end:
    if (fd >= 0)
        my_close(fd, MYF(MY_WME));
    end_io_cache(file);
    return retval;
}

#include "sqlparse.h"

static ulong start_timer(void)
{
#if defined(__WIN__)
    return clock();
#else
    struct tms tms_tmp;
    return times(&tms_tmp);
#endif
}

int
test_remote_server_and_get_definition()
{
    return 0;
}

int main(int argc, char** argv)
{
  char **defaults_argv;
  Exit_status retval= OK_CONTINUE;
  ulonglong save_stop_position;
  int err = 0;
  MY_INIT(argv[0]);
  DBUG_ENTER("main");
  DBUG_PROCESS(argv[0]);

  my_init_time(); // for time functions

  if (load_defaults("my", load_default_groups, &argc, &argv))
    exit(1);
  defaults_argv= argv;
  parse_args(&argc, (char***)&argv);

  if (!argc)
  {
    usage();
    free_defaults(defaults_argv);
    my_end(my_end_arg);
    exit(1);
  }

  if (!sql_files_output_dir || 
      my_access(sql_files_output_dir, F_OK))
  {
      err = -1;
      fprintf(stderr, "Invalid argment --sql-files-output-dir\n");
  }

  if (!write_to_file_only_flag && 
      test_remote_server_and_get_definition())
  {
      //TODO
      //test connect to remote server, and get views trigger functions relations 
      err = -1;
      fprintf(stderr, "Can't connect to remote server\n");
  }

  if (table_split)
  {
    //TODO
  }

  if (err)
  {
      free_defaults(defaults_argv);
      my_end(my_end_arg);
      exit(1);
  }

  if (opt_base64_output_mode == BASE64_OUTPUT_UNSPEC)
    opt_base64_output_mode= BASE64_OUTPUT_AUTO;
  if (opt_base64_output_mode == BASE64_OUTPUT_ALWAYS)
    warning("The --base64-output=always flag and the --base64-output flag "
            "(with '=MODE' omitted), are deprecated. "
            "The output generated when these flags are used cannot be "
            "parsed by mysql 5.6.0 and later. "
            "The flags will be removed in a future version. "
            "Please use --base64-output=auto instead.");

  my_set_max_open_files(open_files_limit);

  MY_TMPDIR tmpdir;
  tmpdir.list= 0;
  if (!dirname_for_local_load)
  {
    if (init_tmpdir(&tmpdir, 0))
      exit(1);
    dirname_for_local_load= my_strdup(my_tmpdir(&tmpdir), MY_WME);
  }

  //if (load_processor.init())
  //  exit(1);
  //if (dirname_for_local_load)
  //  load_processor.init_by_dir_name(dirname_for_local_load);
  //else
  //  load_processor.init_by_cur_dir();

  /* �������½����ӣ��ⲿ�ֿ��Բ����� */
  //fprintf(result_file,
	 // "/*!40019 SET @@session.max_insert_delayed_threads=0*/;\n");

  //if (disable_log_bin)
  //  fprintf(result_file,
  //          "/*!32316 SET @OLD_SQL_LOG_BIN=@@SQL_LOG_BIN, SQL_LOG_BIN=0*/;\n");

  ///*
  //  In mysqlbinlog|mysql, don't want mysql to be disconnected after each
  //  transaction (which would be the case with GLOBAL.COMPLETION_TYPE==2).
  //*/
  //fprintf(result_file,
  //        "/*!50003 SET @OLD_COMPLETION_TYPE=@@COMPLETION_TYPE,"
  //        "COMPLETION_TYPE=0*/;\n");

  //if (charset)
  //  fprintf(result_file,
  //          "\n/*!40101 SET @OLD_CHARACTER_SET_CLIENT=@@CHARACTER_SET_CLIENT */;"
  //          "\n/*!40101 SET @OLD_CHARACTER_SET_RESULTS=@@CHARACTER_SET_RESULTS */;"
  //          "\n/*!40101 SET @OLD_COLLATION_CONNECTION=@@COLLATION_CONNECTION */;"  
  //          "\n/*!40101 SET NAMES %s */;\n", charset);

  //time_t t = time();
  long start = start_timer();
  binlogex_init();

  for (save_stop_position= stop_position, stop_position= ~(my_off_t)0 ;
       (--argc >= 0) ; )
  {
    if (argc == 0) // last log, --stop-position applies
      stop_position= save_stop_position;
    if ((retval= dump_log_entries(*argv++)) != OK_CONTINUE)
      break;

    // For next log, --start-position does not apply
    start_position= BIN_LOG_HEADER_SIZE;
  }

  binlogex_wait_all_worker_thread_exit();

  fprintf(stdout, "Total use %f sec\n", (float)(start_timer() - start) / 1000);

  /*
    Issue a ROLLBACK in case the last printed binlog was crashed and had half
    of transaction.
  */
  //fprintf(result_file,
  //        "# End of log file\nROLLBACK /* added by mysqlbinlog */;\n"
  //        "/*!50003 SET COMPLETION_TYPE=@OLD_COMPLETION_TYPE*/;\n");
  //if (disable_log_bin)
  //  fprintf(result_file, "/*!32316 SET SQL_LOG_BIN=@OLD_SQL_LOG_BIN*/;\n");

  //if (charset)
  //  fprintf(result_file,
  //          "/*!40101 SET CHARACTER_SET_CLIENT=@OLD_CHARACTER_SET_CLIENT */;\n"
  //          "/*!40101 SET CHARACTER_SET_RESULTS=@OLD_CHARACTER_SET_RESULTS */;\n"
  //          "/*!40101 SET COLLATION_CONNECTION=@OLD_COLLATION_CONNECTION */;\n");

  if (tmpdir.list)
    free_tmpdir(&tmpdir);
  //if (result_file != stdout)
  //  my_fclose(result_file, MYF(0));
  cleanup();
  free_defaults(defaults_argv);
  my_free_open_file_info();
  //load_processor.destroy();
  /* We cannot free DBUG, it is used in global destructors after exit(). */

  // no need to wait thread exited
  my_thread_end_wait_time = 0;
  my_end(my_end_arg | MY_DONT_FREE_DBUG);

  binlogex_destroy();

  exit(retval == ERROR_STOP ? 1 : 0);
  /* Keep compilers happy. */
  DBUG_RETURN(retval == ERROR_STOP ? 1 : 0);
}

/*
  We must include this here as it's compiled with different options for
  the server
*/

#include "my_decimal.h"
#include "decimal.c"
#include "my_decimal.cc"
#include "log_event.cc"
#include "log_event_old.cc"
#include "rpl_utility.cc"


/////////////////////////////////////////
#include "message_queue.h"
#include "mysql/psi/mysql_thread.h"

HASH table_hash;
CMessageQueue*  MQ_ARRAY;

static CHARSET_INFO *charset_info= &my_charset_latin1;

uchar* get_table_key(const char *entry, size_t *length,
                     my_bool not_used __attribute__((unused)))
{
    *length= strlen(entry);
    return (uchar*) entry;
}

parse_result_t  parse_result;
my_bool parse_result_inited = 0;
DYNAMIC_ARRAY global_session_event_array; /* �Ự���¼����� */
DYNAMIC_ARRAY global_load_event_array;  /* load�¼������� */
table_mapping table_map_ignored;  /* ����DB�м�binlog��ӳ������Լ����� */
DYNAMIC_ARRAY global_table_map_array; /* һ�������table_map��table_id��thread_id��ӳ���ϵ */
uint*         global_thread_tmap_flag_array;  /* ��伶binlog���ռ����ͷ�table_map�¼��ı�ǣ�1��ʾ�����ɶ�Ӧ�¼� */
pthread_mutex_t global_lock;                  /* ȫ���� */

pthread_attr_t  worker_attrib;
pthread_t*      global_pthread_array; 


/*
 ֻ���߳�waiting��mysql_event_wait(GET_EVENT(waiting, waitfor))
 ֻ���߳�waitfor��mysql_event_set(GET_EVENT(waitfor,waiting))
*/
#define GET_EVENT(waiting, waitfor) (*(global_event_matrix + (concurrency * (waiting)) + (waitfor)))
mysql_event_t* global_event_matrix; 

unsigned global_n_add_to_queue_fail = 0;

Worker_vm** global_vm_array;

struct table_map_ex_struct 
{
    ulong table_id;
    uint thread_id;
};

typedef table_map_ex_struct table_map_ex_t;

#define INVALID_THREAD_ID (uint)(-1)

/*****  table_entry   ******/

struct table_entry_struct 
{
    char full_table_name[NAME_LEN * 2 + 3];      // �����ǵ�һ����Ա
    uint thread_id;
};

typedef struct table_entry_struct table_entry_t;


table_entry_t*
binlogex_new_table_entry(
    const char*       dbname,
    const char*       tablename,
    uint              thread_id_hint      /* hint if not INVALID_THREAD_ID */
)
{
    table_entry_t*  entry = NULL;

    my_assert(strlen(dbname) < NAME_LEN);
    my_assert(strlen(tablename) < NAME_LEN);
    my_assert(thread_id_hint == INVALID_THREAD_ID || thread_id_hint < concurrency); // must less than concurrency

    entry = (table_entry_t*)calloc(1, sizeof(*entry));
    my_assert(entry);

    sprintf(entry->full_table_name, "%s.%s", dbname, tablename);

    if (thread_id_hint != INVALID_THREAD_ID)
        entry->thread_id = thread_id_hint;
    else
    {
        uint i;
        uint folder = 0;
        uint len = strlen(entry->full_table_name);

        for (i = 0; i < len ;++i)
            folder += entry->full_table_name[i] ^ 0xabcd;

        entry->thread_id = folder % concurrency;  // TODO: --table-split
    }

    return entry;
}

/*****  table_entry   ******/

uint
binlogex_get_thread_id_by_name(
    const char*       dbname,
    const char*       tblname
)
{
    char buf[2*NAME_LEN + 3];
    table_entry_t* entry;

    my_assert(strlen(dbname) < NAME_LEN);
    my_assert(strlen(tblname) < NAME_LEN);

    sprintf(buf, "%s.%s", dbname, tblname);

    entry = (table_entry_t*)my_hash_search(&table_hash, (const uchar*)&buf[0], strlen(buf));
    if (entry == NULL)
    {
        entry = binlogex_new_table_entry(dbname, tblname, INVALID_THREAD_ID);
        my_assert((char*)entry == &entry->full_table_name[0]);
        my_bool ret = my_hash_insert(&table_hash, (const uchar*)&entry->full_table_name[0]);
        my_assert(!ret); 
    }

    return entry->thread_id;
}

uint binlogex_get_thread_id_by_tablemap_id(
    ulong table_id
)
{
    uint i;
    table_map_ex_t  tmap_ex;

    for (i = 0; i < global_table_map_array.elements; i++)
    {
        get_dynamic(&global_table_map_array, (uchar*)&tmap_ex, i);
        
        if (tmap_ex.table_id == table_id)
            return tmap_ex.thread_id;
    }
    
    return INVALID_THREAD_ID;
}

/****************** task_entry **********************/

#define TASK_ENTRY_TYPE_EVENT   0
#define TASK_ENTRY_TYPE_SYNC    1
#define TASK_ENTRY_TYPE_ROW_STMT_END 2
#define TASK_ENTRY_TYPE_COMPLETE 3

struct task_entry_struct
{
    uint            type;
    ulonglong       id;

    uint            thread_id;  /* ��ǰ�߳�ID */
    //parse_table_t*  

    union {
        struct {
            Log_event*      ev;

            my_off_t        off;
            char*           log_file;
            uint            n_thread_id;
            uint            n_thread_id_alloced;
            uint*           thread_id_arr;

            uint            dst_thread_id;      // Ŀ��thread_id����ִ�и��¼����̣߳������̸߳���ͬ�����ɡ�        
        } event;

        struct {
            uint            wait_thread_id;
            my_off_t        off;
            char*           log_file;
        } sync;
    } ui;

};

typedef struct task_entry_struct task_entry_t;

task_entry_t*
binlogex_task_entry_event_new(
    ulonglong       id,
    Log_event*      ev,
    const char*     log_file,
    my_off_t        off,
    uint            max_n_thread_id
)
{
    task_entry_t* entry;
    uint size;

    /* �漰�߳���಻��������������Ϊ0��ʾ�����ǿ⼶���� */
    if (max_n_thread_id == 0 || max_n_thread_id > concurrency)
        max_n_thread_id = concurrency;

    // becareful!
    size = ALIGN_SIZE(sizeof(*entry)) + ALIGN_SIZE(strlen(log_file) + 1) +
        max_n_thread_id * sizeof(*entry->ui.event.thread_id_arr);

    //entry = (task_entry_t*)calloc(1, sizeof(*entry));
    entry = (task_entry_t*)malloc(size);
    memset(entry, 0, size);

    entry->id = id;
    entry->type = TASK_ENTRY_TYPE_EVENT;
    entry->thread_id = INVALID_THREAD_ID;

    entry->ui.event.ev = ev;
    entry->ui.event.off = off;
    entry->ui.event.log_file = strcpy((char*)entry + ALIGN_SIZE(sizeof(*entry)), log_file);
    entry->ui.event.n_thread_id_alloced = max_n_thread_id;
    entry->ui.event.thread_id_arr = (uint*)((char*)entry + ALIGN_SIZE(sizeof(*entry)) + ALIGN_SIZE(strlen(log_file) + 1));
    //entry->ui.event.thread_id_arr = (uint*)calloc(max_n_thread_id, sizeof(*entry->ui.event.thread_id_arr));

    return entry;
}

task_entry_t*
binlogex_task_entry_sync_new(
    task_entry_t*           event_entry
)
{
    task_entry_t* entry;
    char*   log;

    my_assert(event_entry->type == TASK_ENTRY_TYPE_EVENT);
    // not add to queue yet
    //my_assert(event_entry->thread_id == event_entry->ui.event.dst_thread_id);
    my_assert(event_entry->ui.event.dst_thread_id != INVALID_THREAD_ID);

    log = event_entry->ui.event.log_file;
    entry = (task_entry_t*)calloc(1, ALIGN_SIZE(sizeof(*entry)) + ALIGN_SIZE(strlen(log) + 1));

    entry->id = event_entry->id;
    entry->type = TASK_ENTRY_TYPE_SYNC;
    entry->thread_id = INVALID_THREAD_ID; // set in binlogex_task_entry_add_to_queue

    entry->ui.sync.wait_thread_id = event_entry->ui.event.dst_thread_id;
    entry->ui.sync.off = event_entry->ui.event.off;
    entry->ui.sync.log_file = (char*)entry + ALIGN_SIZE(sizeof(*entry));
    strcpy(entry->ui.sync.log_file, log);

    return entry;

}

void
binlogex_task_entry_destroy(
    task_entry_t*       entry                            
)
{
    if(!entry)
        return;
/*
    if (entry->type == TASK_ENTRY_TYPE_EVENT)
    {
        if (entry->ui.event.thread_id_arr) 
        {
            free(entry->ui.event.thread_id_arr);
            entry->ui.event.thread_id_arr = NULL;
        }
    }
*/
    free(entry);
}

void
binlogex_task_entry_event_add_thread_id(
    task_entry_t*           entry,
    uint                    thread_id
)
{
    uint i = 0;
    
    my_assert(entry->type == TASK_ENTRY_TYPE_EVENT);

    for (i = 0; i < entry->ui.event.n_thread_id; ++i)
    {
        if (thread_id == entry->ui.event.thread_id_arr[i])
            return;
    }

    my_assert(entry->ui.event.n_thread_id < entry->ui.event.n_thread_id_alloced);

    entry->ui.event.thread_id_arr[entry->ui.event.n_thread_id++] = thread_id;

}
   
inline
void
binlogex_task_entry_event_set_dst_thread_id(
    task_entry_t*           entry,
    uint                    thread_id
)
{
    my_assert(entry->type == TASK_ENTRY_TYPE_EVENT);

    if (entry->ui.event.n_thread_id == 1 &&
         entry->ui.event.thread_id_arr[0] == INVALID_THREAD_ID)
    {
        entry->ui.event.thread_id_arr[0] = thread_id;
    }

    entry->ui.event.dst_thread_id = thread_id;
}

inline
void
binlogex_task_entry_event_decide_dst_thread_id(
    task_entry_t*           entry
)
{
    my_assert(entry->type == TASK_ENTRY_TYPE_EVENT);

    //TODO
    binlogex_task_entry_event_set_dst_thread_id(entry, entry->ui.event.thread_id_arr[0]);
}

task_entry_t*
binlogex_task_entry_event_simple_new(
    ulonglong       id,
    Log_event*      ev,
    const char*     log_file,
    my_off_t        off,
    uint            thread_id
)
{
    task_entry_t*   entry;

    entry = binlogex_task_entry_event_new(id, ev, log_file, off, 1);

    binlogex_task_entry_event_add_thread_id(entry, thread_id);

    binlogex_task_entry_event_set_dst_thread_id(entry, thread_id);

    return entry;
}

task_entry_t*
binlogex_task_entry_stmt_end_new(
    ulonglong       id
)
{
    task_entry_t* entry;

    entry = (task_entry_t*)calloc(1, sizeof(*entry));

    entry->id = id;
    entry->type = TASK_ENTRY_TYPE_ROW_STMT_END;
    entry->thread_id = INVALID_THREAD_ID;

    return entry;
}

task_entry_t*
binlogex_task_entry_complete_new()
{
    task_entry_t* entry;

    entry = (task_entry_t*)calloc(1, sizeof(*entry));

    entry->id = (ulonglong)-1;
    entry->type = TASK_ENTRY_TYPE_COMPLETE;
    entry->thread_id = INVALID_THREAD_ID;

    return entry;
}

my_bool
binlogex_task_entry_is_event(
    task_entry_t*       entry
)
{
    my_assert(entry);
    return entry->type == TASK_ENTRY_TYPE_EVENT;
}


my_bool
binlogex_task_entry_add_to_queue(
    task_entry_t*           task_entry,
    uint                    thread_id
)
{
    uint n_retry = 0;
    my_assert(task_entry != NULL && thread_id != INVALID_THREAD_ID);
    my_assert(task_entry->type != TASK_ENTRY_TYPE_EVENT || 
            thread_id == task_entry->ui.event.dst_thread_id);

    my_assert(task_entry->thread_id == INVALID_THREAD_ID);
    my_assert(thread_id != INVALID_THREAD_ID);

    task_entry->thread_id = thread_id;

    //���뵽��Ӧ��������У������Ӧ����������ȴ�
    while (MQ_ARRAY[thread_id].send_message(0, task_entry, NULL, NULL, NULL))
    {
        /* �ȴ�100���� */
        my_sleep(100000);

        if (++n_retry % 10 == 0)
            fprintf(stderr, "Waiting Task Entry add to MQ[%u], retry count: %u, global retry count: %u\n", thread_id, n_retry, global_n_add_to_queue_fail);

        if (++global_n_add_to_queue_fail % 100 == 0)
            fprintf(stderr, "Now Waiting Task Entry add to MQ[%u], retry count: %u, global retry count: %u\n", thread_id, n_retry, global_n_add_to_queue_fail);

    }
        
    return 0;
}

/**********  worker thread **************/

enum Worker_status {
    WORKER_STATUS_OK,
    WORKER_STATUS_ERROR,
    WORKER_STATUS_EXIT
};

FILE* my_popen(
    const char*       cmd,
    const char*       modes 
)
{
#ifdef __WIN__
    return _popen(cmd, modes);
#else
    return popen(cmd, modes);
#endif // __WIN__
}

int my_pclose(
    FILE*       file 
)
{
#ifdef __WIN__
    return _pclose(file);
#else
    return pclose(file);
#endif // __WIN__
}

#define RET_IF_ERR(ret) \
    if (ret < 0) { my_assert(0); return -1; }


int
binlogex_worker_thread_init(
    Worker_vm*          vm
)
{
    char file_name[1024] = {0};
    char tmp_file_name[1024] = {0};
    int ret;
    int len;

    snprintf(tmp_file_name, sizeof(tmp_file_name), "%s/tmp_%u.log", sql_files_output_dir, vm->thread_id);  
    vm->tmp_file = my_fopen(tmp_file_name, O_WRONLY | O_BINARY, MYF(MY_WME));

    len = snprintf(file_name, sizeof(file_name), "%s/thread_%u.sql", sql_files_output_dir, vm->thread_id);  
    if (len == sizeof(file_name) - 1 || len < 0)
    {
        fprintf(stderr, "sql_files_output_dir too long\n");
        return -1;
    }

    if (write_to_file_only_flag)
    {
        //TODO
        vm->result_file = my_fopen(file_name, O_WRONLY | O_BINARY, MYF(MY_WME));
    }
    else
    {
        char mysql_cmd[1024] = {0};
        char cmd[2048] = {0};

        //TODO 
        snprintf(mysql_cmd, sizeof(mysql_cmd), "%s -u%s %s%s -h%s -P%u %s > %s/mysql_%u.log 2>&1 ", 
                    mysql_path, remote_user, 
                    strlen(remote_pass) == 0 ? "" : "-p" , remote_pass,  
                    remote_host, remote_port, 
                    exit_when_error ? "" : "-f", 
                    sql_files_output_dir, vm->thread_id); 
#ifdef __WIN__
        strcpy(cmd, mysql_cmd);
#else
        snprintf(cmd, sizeof(cmd), "tee %s |%s", file_name,mysql_cmd);
#endif // __WIN__

        vm->result_file = my_popen(cmd, "w");
    }
    
    if (!vm->result_file)
    {
        fprintf(stderr, "[ERROR] Can't open file %s\n", file_name);
        return -1;
    }

    ret = fprintf(vm->result_file,
        "/*!40019 SET @@session.max_insert_delayed_threads=0*/;\n");
    RET_IF_ERR(ret);


    if (disable_log_bin) 
    {
        ret = fprintf(vm->result_file,
            "/*!32316 SET @OLD_SQL_LOG_BIN=@@SQL_LOG_BIN, SQL_LOG_BIN=0*/;\n");
        RET_IF_ERR(ret);
    }

    /*
    In mysqlbinlog|mysql, don't want mysql to be disconnected after each
    transaction (which would be the case with GLOBAL.COMPLETION_TYPE==2).
    */
    ret = fprintf(vm->result_file,
        "/*!50003 SET @OLD_COMPLETION_TYPE=@@COMPLETION_TYPE,"
        "COMPLETION_TYPE=0*/;\n");
    RET_IF_ERR(ret);

    if (charset) 
    {
        ret = fprintf(vm->result_file,
            "\n/*!40101 SET @OLD_CHARACTER_SET_CLIENT=@@CHARACTER_SET_CLIENT */;"
            "\n/*!40101 SET @OLD_CHARACTER_SET_RESULTS=@@CHARACTER_SET_RESULTS */;"
            "\n/*!40101 SET @OLD_COLLATION_CONNECTION=@@COLLATION_CONNECTION */;"  
            "\n/*!40101 SET NAMES %s */;\n", charset);
        RET_IF_ERR(ret);
    }
    ret = fprintf(vm->result_file, "DELIMITER /*!*/;\n");
    RET_IF_ERR(ret);
    
    if (!vm->print_info.init_ok())
          my_assert(0);
    /*
     Set safe delimiter, to dump things
     like CREATE PROCEDURE safely
    */
    strmov(vm->print_info.delimiter, "/*!*/;");

    vm->print_info.verbose= short_form ? 0 : verbose;

    if (vm->load_processor.init())
    {
        fprintf(stderr, "[ERROR] load_prodessor init failed\n");
        return -1;
    }
    if (dirname_for_local_load)
        vm->load_processor.init_by_dir_name(dirname_for_local_load);
    else
        vm->load_processor.init_by_cur_dir();

    return 0;
}

int
binlogex_worker_thread_deinit(
    Worker_vm*              vm
)
{
    int ret;
    vm->load_processor.destroy();

    strmov(vm->print_info.delimiter, ";");

    ret = fprintf(vm->result_file, "DELIMITER ;\n");
    RET_IF_ERR(ret);

    /*
    Issue a ROLLBACK in case the last printed binlog was crashed and had half
    of transaction.
    */
    ret = fprintf(vm->result_file,
        "# End of log file\nROLLBACK /* added by mysqlbinlog */;\n"
        "/*!50003 SET COMPLETION_TYPE=@OLD_COMPLETION_TYPE*/;\n");
    RET_IF_ERR(ret);

    if (disable_log_bin)
    {
        ret = fprintf(vm->result_file, "/*!32316 SET SQL_LOG_BIN=@OLD_SQL_LOG_BIN*/;\n");
        RET_IF_ERR(ret);
    }
    if (charset)
    {
        ret = fprintf(vm->result_file,
            "/*!40101 SET CHARACTER_SET_CLIENT=@OLD_CHARACTER_SET_CLIENT */;\n"
            "/*!40101 SET CHARACTER_SET_RESULTS=@OLD_CHARACTER_SET_RESULTS */;\n"
            "/*!40101 SET COLLATION_CONNECTION=@OLD_COLLATION_CONNECTION */;\n");
        RET_IF_ERR(ret);
    }
   
    if (write_to_file_only_flag)
        my_fclose(vm->result_file, MYF(0));
    else
        my_pclose(vm->result_file);

    my_fclose(vm->tmp_file, MYF(0));

    return 0;
}

/* waiting_thread_id �ȴ� waitfor_tread_id  */
void
binlogex_worker_wait(
    Worker_vm*          vm,
    task_entry_t*       tsk_entry,
    uint                waiting_thread_id,
    uint                waitfor_thread_id
)
{
    mysql_event_t event;

    if (tsk_entry && tsk_entry->type == TASK_ENTRY_TYPE_SYNC)
    {
        fprintf(vm->result_file, "# [tmysqlbinlogex] Thread %u is waiting for thread %u(W[%u][%u]), logfile %s offset %u\n", 
                waiting_thread_id, waitfor_thread_id, 
                waiting_thread_id, waitfor_thread_id, 
                tsk_entry->ui.sync.log_file, (unsigned int)tsk_entry->ui.sync.off);
    }
    else
    {
        fprintf(vm->result_file, "# [tmysqlbinlogex] Thread %u is waiting for thread %u(W[%u][%u])\n", 
            waiting_thread_id, waitfor_thread_id, 
            waiting_thread_id, waitfor_thread_id);
    }
    

    if (write_to_file_only_flag)
    {
        //return;
    }

    /* ��ȡ���ʵ�event */
    event = GET_EVENT(waiting_thread_id, waitfor_thread_id);
    if (event == NULL)
    {
        pthread_mutex_lock(&global_lock);

        event = GET_EVENT(waiting_thread_id, waitfor_thread_id);
        if (event == NULL)
        {
            GET_EVENT(waiting_thread_id, waitfor_thread_id) = mysql_event_create(NULL);
            event = GET_EVENT(waiting_thread_id, waitfor_thread_id);
            my_assert(event);
        }

        pthread_mutex_unlock(&global_lock);
    }

    mysql_event_wait(event);

    mysql_event_reset(event);
}

/* singal_tread_id ���� waiting_thread_id */
void
binlogex_worker_signal(
    Worker_vm*      vm,
    task_entry_t*   tsk_entry,
    uint            signal_thread_id,
    uint            waiting_thread_id
)
{
    mysql_event_t event;

    if (tsk_entry && tsk_entry->type == TASK_ENTRY_TYPE_SYNC)
    {
        fprintf(vm->result_file, "# [tmysqlbinlogex] Thread %u waiked up thread %u(S[%u][%u]), logfile %s offset %u\n", 
            signal_thread_id, waiting_thread_id, 
            waiting_thread_id, signal_thread_id,
            tsk_entry->ui.sync.log_file, (unsigned)tsk_entry->ui.sync.off);
    }
    else
    {
        fprintf(vm->result_file, "# [tmysqlbinlogex] Thread %u waked up thread %u(S[%u][%u])\n", 
            signal_thread_id, waiting_thread_id,
            waiting_thread_id, signal_thread_id);
    }

    if (write_to_file_only_flag)
    {
        //return;
    }

    /* ��ȡ���ʵ�event */
    event = GET_EVENT(waiting_thread_id, signal_thread_id);
    if (event == NULL)
    {
        /* ������binlogex_worker_signal�Ĳ������ */
        pthread_mutex_lock(&global_lock);

        event = GET_EVENT(waiting_thread_id, signal_thread_id);
        if (event == NULL)
        {
            GET_EVENT(waiting_thread_id, signal_thread_id) = mysql_event_create(NULL);
            event = GET_EVENT(waiting_thread_id, signal_thread_id);
            assert(event);
        }

        pthread_mutex_unlock(&global_lock);
    }

    mysql_event_set(event);

}

Worker_status
binlogex_worker_execute_task_entry(
    Worker_vm*      vm,
    task_entry_t*   tsk_entry
)
{
    uint i;
    Worker_status code = WORKER_STATUS_OK;

    my_assert(vm->thread_id == tsk_entry->thread_id);

    switch (tsk_entry->type)
    {
    case TASK_ENTRY_TYPE_EVENT:
    {
        Log_event*  ev;
        ev = tsk_entry->ui.event.ev;

        my_assert(tsk_entry->thread_id == tsk_entry->ui.event.dst_thread_id);

        /* �ȴ������̸߳�������ǰ��������� */
        for (i = 0; i < tsk_entry->ui.event.n_thread_id; i++)
        {
            uint        waitfor_thread_id;
            waitfor_thread_id = tsk_entry->ui.event.thread_id_arr[i];

            if (waitfor_thread_id != tsk_entry->thread_id)
                binlogex_worker_wait(vm, tsk_entry, tsk_entry->thread_id, waitfor_thread_id); 
        }

        /* ִ������ */
        if (process_event(vm, ev, tsk_entry->ui.event.off, tsk_entry->ui.event.log_file) != OK_CONTINUE)
        {
            //my_assert(0);
            fprintf(stderr, "[ERROR] Thread %u execute binlog error at %s pos %u\n", vm->thread_id, tsk_entry->ui.event.log_file, (ulong)tsk_entry->ui.event.off);
            code = WORKER_STATUS_ERROR;
        }

        fprintf(vm->tmp_file, "%s %u before flush %u ", tsk_entry->ui.event.log_file, (ulong)tsk_entry->ui.event.off, start_timer());

        /* SQL�����Ի�����buffer�У�fflushʹ������ִ�У���eventΪ��λ */
        fflush(vm->result_file);

        fprintf(vm->tmp_file, "after flush %u\n", start_timer());

        /* ������ɺ��������߳� */
        for (i = 0; i < tsk_entry->ui.event.n_thread_id; i++)
        {
            uint        waiting_thread_id;
            waiting_thread_id = tsk_entry->ui.event.thread_id_arr[i];

            if (waiting_thread_id != tsk_entry->thread_id)
                binlogex_worker_signal(vm, tsk_entry, tsk_entry->thread_id, waiting_thread_id); 
        }

        break;
    }
    case TASK_ENTRY_TYPE_SYNC:
    {
        uint        wait_thread_id;
        wait_thread_id = tsk_entry->ui.sync.wait_thread_id;
        
        /* ���ѵȴ���ִ�������߳� */
        binlogex_worker_signal(vm, tsk_entry, tsk_entry->thread_id, wait_thread_id);
        /* �ȴ�ִ��������߳̽��� */
        binlogex_worker_wait(vm, tsk_entry, tsk_entry->thread_id, wait_thread_id);

        break;
    }

    case TASK_ENTRY_TYPE_ROW_STMT_END:
    {
        //do like process_event STMT_END_F

        if (vm->print_info.base64_output_mode != BASE64_OUTPUT_DECODE_ROWS)
        {
            my_b_printf(&vm->print_info.body_cache, "'%s\n", vm->print_info.delimiter);
            //fprintf(vm->result_file, "'%s\n", vm->print_info.delimiter);
        }

        if ((copy_event_cache_to_file_and_reinit(&vm->print_info.head_cache, vm->result_file) ||
             copy_event_cache_to_file_and_reinit(&vm->print_info.body_cache, vm->result_file)))
            code = WORKER_STATUS_ERROR;

        fprintf(vm->result_file, "# [tmysqlbinlogex] TASK_ENTRY_TYPE_ROW_STMT_END\n");

        /* SQL�����Ի�����buffer�У�fflushʹ������ִ�У���eventΪ��λ */
        fflush(vm->result_file);

        break;
    }

    case TASK_ENTRY_TYPE_COMPLETE:
    {
        code = WORKER_STATUS_EXIT;
        break;
    }

    default:
        my_assert(0);
        break;;
    }

    binlogex_task_entry_destroy(tsk_entry);

    return code;
}


pthread_handler_t 
binlogex_worker_thread(void *arg)
{
    uint thread_id =0;
    Message msg;
    uint    n_retry = 0;
    task_entry_t*   tsk_entry;
    Worker_vm*  vm = (Worker_vm*)arg;
    Worker_status   status;
    my_bool     is_over = FALSE;
    my_bool     write_file = TRUE;

    my_thread_init();
    if(binlogex_worker_thread_init(vm))
    {
        fprintf(stderr, "[ERROR] Thread %u init error", vm->thread_id);
        exit(-1);
    }

    thread_id = vm->thread_id;

    while(1)
    {
        /* ������� */
        while (MQ_ARRAY[thread_id].get_message(msg))
        {
            if (++n_retry % 10 == 0)
                fprintf(stderr, "Worker thread %u wait count %u, each wait sleep 1 ms\n", thread_id, n_retry);

            //my_sleep(2000000);
            my_sleep(1000);
        } 

        my_assert(msg.Msg != -1);
        tsk_entry = (task_entry_t*)msg.PARAM_1;
        my_assert(tsk_entry);

        status = binlogex_worker_execute_task_entry(vm, tsk_entry);
        switch (status)
        {
        case WORKER_STATUS_ERROR:
            my_assert(0);
            break;
        case WORKER_STATUS_EXIT:
            is_over = TRUE;
            break;
        default:
            break;
        }

        if (is_over)
            break;
    }

    binlogex_worker_thread_deinit(vm);

    pthread_exit(0);
    return 0;
}

void
binlogex_wait_all_worker_thread_exit()
{
    uint i = 0;

    for (i = 0; i < concurrency; i++)
    {
        binlogex_task_entry_add_to_queue(binlogex_task_entry_complete_new(), i);

        pthread_join(global_pthread_array[i], NULL);
    }
    
}

/**********  worker thread **************/



int
binlogex_init()
{
    uint i;

    if (my_hash_init(&table_hash, charset_info, 64, 0, 0,
            (my_hash_get_key) get_table_key, my_free, 0))
    {
        my_assert(0);
        return -1;
    }

    parse_global_init();

    my_init_dynamic_array(&global_session_event_array, sizeof(Log_event*), 16, 16);
    my_init_dynamic_array(&global_load_event_array, sizeof(Log_event*), 16, 16);
    my_init_dynamic_array(&global_table_map_array, sizeof(table_map_ex_t), 16, 16);

    concurrency = 4;

    global_thread_tmap_flag_array = (uint*)calloc(concurrency, sizeof(uint));

    MQ_ARRAY = new CMessageQueue[concurrency]();

    global_event_matrix = (mysql_event_t*)calloc(concurrency * concurrency, sizeof(mysql_event_t));

    pthread_attr_init(&worker_attrib);
    //pthread_attr_setdetachstate(&worker_attrib, PTHREAD_CREATE_DETACHED);
    pthread_attr_setscope(&worker_attrib, PTHREAD_SCOPE_SYSTEM);

    global_pthread_array = (pthread_t*)calloc(concurrency, sizeof(pthread_t));

    pthread_mutex_init(&global_lock, NULL);
    global_vm_array = (Worker_vm**)calloc(concurrency, sizeof(Worker_vm*));
    for (i = 0; i < concurrency; ++i)
    {
        global_vm_array[i] = new Worker_vm(i);
        pthread_create(&global_pthread_array[i], &worker_attrib, binlogex_worker_thread, global_vm_array[i]);
    }

    return 0;
}

void
binlogex_destroy()
{
    uint i, j;

    parse_result_destroy(&parse_result);

    my_hash_free(&table_hash);
    delete_dynamic(&global_session_event_array);
    delete_dynamic(&global_load_event_array);
    delete_dynamic(&global_table_map_array);

    free(global_thread_tmap_flag_array);

    delete [] MQ_ARRAY;

    for (i = 0; i < concurrency; ++i)
    {
        for (j = 0; j < concurrency; ++j)
        {
            mysql_event_t event = GET_EVENT(i, j);

            if (event)
                mysql_event_free(event);
        }
    }
    free(global_event_matrix);
    
    for (i = 0; i < concurrency; ++i)
        delete global_vm_array[i];

    free(global_vm_array);
    free(global_pthread_array);

    pthread_mutex_destroy(&global_lock);

    parse_global_destroy();
}


/////////////////////////////////////////////////////////////////////
//���̷ַ߳�����
Exit_status binlogex_process_event(Log_event *ev,
                          my_off_t pos, const char *logname)
{
  //char ll_buff[21];
  Log_event_type ev_type= ev->get_type_code();
  my_bool destroy_evt= TRUE;
  DBUG_ENTER("process_event");
  //print_event_info->short_form= short_form;
  Exit_status retval= OK_CONTINUE;
  uint i; 
  uint                thread_id = 0;
  task_entry_t*       tsk_entry;
  uint                parse_ret;

  if (!parse_result_inited)
  {
      if (parse_result_init(&parse_result))
      {
          fprintf(stderr, "[ERROR] parse_result_init failed\n");
          return OK_STOP;
      }

      parse_result_inited = 1;
  }

  if (rec_count % 1024 == 0)
  {
      /* ����������ú��ͷſ���ռ�ô�������Դ */
      parse_result_destroy(&parse_result);
      if (parse_result_init(&parse_result))
      {
          parse_result_inited = 0;
          fprintf(stderr, "[ERROR] parse_result_init failed, rec_count %d\n", rec_count);
          return OK_STOP;
      }
      parse_result_inited = 1;
  }

  /*
    Format events are not concerned by --offset and such, we always need to
    read them to be able to process the wanted events.
  */
  if (((rec_count >= offset) &&
       ((my_time_t)(ev->when) >= start_datetime)) ||
      (ev_type == FORMAT_DESCRIPTION_EVENT))
  {
    if (ev_type != FORMAT_DESCRIPTION_EVENT)
    {
      /*
        We have found an event after start_datetime, from now on print
        everything (in case the binlog has timestamps increasing and
        decreasing, we do this to avoid cutting the middle).
      */
      start_datetime= 0;
      offset= 0; // print everything and protect against cycling rec_count
      /*
        Skip events according to the --server-id flag.  However, don't
        skip format_description or rotate events, because they they
        are really "global" events that are relevant for the entire
        binlog, even if they have a server_id.  Also, we have to read
        the format_description event so that we can parse subsequent
        events.
      */
      if (ev_type != ROTATE_EVENT &&
          server_id && (server_id != ev->server_id))
        goto end;
    }
    if (((my_time_t)(ev->when) >= stop_datetime)
        || (pos >= stop_position_mot))
    {
      /* end the program */
      retval= OK_STOP;
      goto end;
    }

    //if (!short_form)
    //  fprintf(result_file, "# at %s\n",llstr(pos,ll_buff));

    //if (!opt_hexdump)
    //  print_event_info->hexdump_from= 0; /* Disabled */
    //else
    //  print_event_info->hexdump_from= pos;

    //print_event_info->base64_output_mode= opt_base64_output_mode;

    DBUG_PRINT("debug", ("event_type: %s", ev->get_type_str()));

    switch (ev_type) {
    case FORMAT_DESCRIPTION_EVENT:
        delete glob_description_event;
        glob_description_event= (Format_description_log_event*) ev;
        if (!force_if_open_opt &&
            (ev->flags & LOG_EVENT_BINLOG_IN_USE_F))
        {
            error("Attempting to dump binlog '%s', which was not closed properly. "
                "Most probably, mysqld is still writing it, or it crashed. "
                "Rerun with --force-if-open to ignore this problem.", logname);
            DBUG_RETURN(ERROR_STOP);
        }
        //print_event_info->common_header_len=
         //   glob_description_event->common_header_len;
    case START_EVENT_V3 :
        // ����ַ��������̣߳���Ϊ�м�binlog������������¼�
        for (i = 0; i < concurrency; ++i)
        {
            Log_event* tmp_ev;
            
            if (ev_type == FORMAT_DESCRIPTION_EVENT)
            {
                /* ͵��������Ĭ�Ϲ��캯���� */
                Format_description_log_event* tmp_fev, *fev;
                fev = (Format_description_log_event*)ev;
                tmp_fev = new Format_description_log_event(*fev);

                /* ����Ĭ�Ϲ��캯����ǳ���� */
                tmp_fev->post_header_len= (uint8*) my_memdup((uchar*)fev->post_header_len,
                    fev->number_of_event_types* sizeof(*fev->post_header_len), MYF(0));

                tmp_ev = tmp_fev;
            }
            else
            {
                Start_log_event_v3  *tmp_v3, *v3;

                v3 = (Start_log_event_v3*)ev;
                tmp_v3 = new Start_log_event_v3(*v3);

                tmp_ev = tmp_v3;
            }

            if (tmp_ev->temp_buf)  
            {
                uint32 size= uint4korr(ev->temp_buf + EVENT_LEN_OFFSET);
                tmp_ev->temp_buf = (char*)my_memdup(ev->temp_buf, size, MYF(MY_FAE));
            }

            //TODO: FORMAT_DESCRIPTION_EVENT�ĸ��ƹ��캯����ÿ���߳�һ���������ͷ�
            tsk_entry = binlogex_task_entry_event_simple_new(rec_count, tmp_ev, logname, pos, i);
            if (binlogex_task_entry_add_to_queue(tsk_entry, i))
                my_assert(0);
        }

        break;
    case QUERY_EVENT:
    {
        Query_log_event* query_ev = (Query_log_event*)ev;

        /* TODO, �����м�binlog��begin����ĳЩ��Ϣ����foreign_check�ȣ����ܲ��ܺ��� */
        if (!query_ev->is_trans_keyword())
        {
            // TODO: should session binlog make empty?
            if (shall_skip_database(query_ev->db))
            {
                delete ev;
                break;
            }

            /* ����DB�� */
            parse_result_init_db(&parse_result, (char*)query_ev->db);

            parse_ret = query_parse((char*)query_ev->query, &parse_result);
            my_assert(!parse_ret);

            tsk_entry = binlogex_task_entry_event_new(rec_count, ev, logname, pos, parse_result.n_tables);
            my_assert(tsk_entry);

            if (parse_result.n_tables == 0)
            {
                // ���ܿ⼶���������������߳�
                // �� drop database d1;
                for (i = 0; i < concurrency; ++i)
                {
                    binlogex_task_entry_event_add_thread_id(tsk_entry, i);
                }
            }
            else 
            {
                /* �����漰���߳�id */
                for (i = 0; i < parse_result.n_tables; ++i)
                {
                    thread_id = binlogex_get_thread_id_by_name(parse_result.table_arr[i].dbname, parse_result.table_arr[i].tablename); 

                    binlogex_task_entry_event_add_thread_id(tsk_entry, thread_id);
                }
            }

            /* ȷ����������������߳� */
            binlogex_task_entry_event_decide_dst_thread_id(tsk_entry);

            /* ����global_tmp_event_array��RAND_EVENT�ȣ�,���뵽Ŀ���߳��� */
            for (i = 0; i < global_session_event_array.elements; ++i)
            {
                task_entry_t*   tmp_tsk_entry = NULL;

                get_dynamic(&global_session_event_array, (uchar*)&tmp_tsk_entry, i);
                my_assert(!tmp_tsk_entry && tmp_tsk_entry->type == TASK_ENTRY_TYPE_EVENT);

                binlogex_task_entry_event_set_dst_thread_id(tmp_tsk_entry, thread_id);

                if (binlogex_task_entry_add_to_queue(tmp_tsk_entry, tsk_entry->ui.event.dst_thread_id))
                    my_assert(0);
            }
            /* ���global_tmp_event_array */
            global_session_event_array.elements = 0;

            /* �½�ͬ�����񣬼��뵽�����߳��� */
            for (i = 0; i < tsk_entry->ui.event.n_thread_id; ++i)
            {
                if (tsk_entry->ui.event.thread_id_arr[i] != tsk_entry->ui.event.dst_thread_id)
                {
                    if (binlogex_task_entry_add_to_queue(
                            binlogex_task_entry_sync_new(tsk_entry),
                            tsk_entry->ui.event.thread_id_arr[i]))
                        my_assert(0);
                }
            }

            /* �¼�������뵽�̶߳����� */
            if (binlogex_task_entry_add_to_queue(tsk_entry, tsk_entry->ui.event.dst_thread_id))
                my_assert(0);
        }
        else
        {
            delete ev;
            /* �Ự���¼�Ӧ��ֻ�������begin��commit֮�䣬������shall_skip_database�Ͳ�һ���� */
            //my_assert(global_tmp_event_array.elements == 0);
        }
        break;
    }

    case XID_EVENT:
        //TODO: �Ƿ��п���©���ύ����
        delete ev;
        break;

    case INTVAR_EVENT:
    case RAND_EVENT:
    case USER_VAR_EVENT:
    {
        /* �ü�¼�¼����뵽global_tmp_event_array�� */
        tsk_entry = binlogex_task_entry_event_simple_new(rec_count, ev, logname, pos, INVALID_THREAD_ID);
        insert_dynamic(&global_session_event_array, (uchar*)tsk_entry);
        break;
    }

    /* Load data infile */
    /*
        MySQL 3.23: LOAD_EVENT
        MySQL 4.0.0: CREATE_FILE_EVENT * 1 + APPEND_BLOCK_EVENT *(0~n) + EXEC_LOAD_EVENT/DELETE_FILE_EVENT * 1
                     NEW_LOAD_EVENT
        MySQL 5.0.3: BEGIN_LOAD_QUERY_EVENT * 1 + APPEND_BLOCK_EVENT *(0~n) + EXECUTE_LOAD_QUERY_EVENT/DELETE_FILE_EVENT * 1

        �����Ļ�����DELETE_FILE_EVENT
    */

    case LOAD_EVENT:
    case NEW_LOAD_EVENT:
    {
        Load_log_event* lle = (Load_log_event*)ev;

        thread_id = binlogex_get_thread_id_by_name((char*)lle->db, (char*)lle->table_name);

        tsk_entry = binlogex_task_entry_event_simple_new(rec_count, ev, logname, pos, thread_id);
        binlogex_task_entry_add_to_queue(tsk_entry, thread_id);

        break;
    }

    case EXEC_LOAD_EVENT:
    {
        Log_event* tmp_event;
        uint tmp_file_id = -1;
        Create_file_log_event* cev = NULL;
        Execute_load_log_event* ele = NULL;
        my_bool should_skip = FALSE;
        task_entry_t* tmp_tentry = NULL;

        ele = (Execute_load_log_event*)ev;

        /* �ҵ�CREATE_FILE_EVENT�¼� */
        for (i = 0; i < global_load_event_array.elements; ++i)
        {
            tmp_event = NULL;
            tmp_tentry = NULL;
            get_dynamic(&global_load_event_array, (uchar*)&tmp_tentry, i); 
            my_assert(binlogex_task_entry_is_event(tmp_tentry));

            tmp_event = tmp_tentry->ui.event.ev;

            if(tmp_event->get_type_code() == CREATE_FILE_EVENT)
            {
                cev = (Create_file_log_event*)tmp_event;
                if (cev->file_id == ele->file_id)
                {
                    should_skip = shall_skip_database(cev->db);
                    break;
                }
                cev = NULL;
            }
        }
        if (cev == NULL)
        {
            should_skip = TRUE;
            thread_id = INVALID_THREAD_ID;
            warning("Ignoring Execute_load_event as there is no "
                "Create_file event for file_id: %u", ele->file_id);
        }
        else
            thread_id = binlogex_get_thread_id_by_name((char*)cev->db, (char*)cev->table_name);

        for (i = 0; i < global_load_event_array.elements; ++i)
        {
            tmp_event = NULL;
            tmp_tentry = NULL;
            get_dynamic(&global_load_event_array, (uchar*)&tmp_tentry, i); 
            my_assert(binlogex_task_entry_is_event(tmp_tentry));

            tmp_event = tmp_tentry->ui.event.ev;
            switch(tmp_event->get_type_code())
            {
            case CREATE_FILE_EVENT:
                tmp_file_id = ((Create_file_log_event*)tmp_event)->file_id;
                break;

            case APPEND_BLOCK_EVENT:
                tmp_file_id = ((Append_block_log_event*)tmp_event)->file_id;
                break;

            default:
                my_assert(0);
                break;
            }

            if (tmp_file_id == ele->file_id)
            {
                /* �Ӷ�̬������ɾ�� */
                delete_dynamic_element(&global_load_event_array, i);

                if (!should_skip)
                {
                    binlogex_task_entry_event_set_dst_thread_id(tmp_tentry, thread_id);
                    binlogex_task_entry_add_to_queue(tmp_tentry, thread_id);
                }
                else 
                {
                    binlogex_task_entry_destroy(tmp_tentry);
                    delete tmp_event;
                }
                i--;
            }
        }
        if (should_skip)
            delete ev;
        
        break;
    }

    case CREATE_FILE_EVENT:
    case BEGIN_LOAD_QUERY_EVENT:
        /* �ü�¼�¼����뵽global_load_event_array�� */
        tsk_entry = binlogex_task_entry_event_simple_new(rec_count, ev, logname, pos, INVALID_THREAD_ID);
        insert_dynamic(&global_load_event_array, (uchar*)tsk_entry);
        break;

    case APPEND_BLOCK_EVENT:
    {
        Log_event* tmp_event;
        task_entry_t* tmp_tsk_entry = NULL;
        uint tmp_file_id = -1;

        /* ����֮ǰ�Ѵ���BEGIN_LOAD_QUERY_EVENT��CREATE_FILE_EVENT�¼� */
        for (i = 0; i < global_load_event_array.elements; ++i)
        {
            tmp_event = NULL;
            tmp_tsk_entry = NULL;
            get_dynamic(&global_load_event_array, (uchar*)&tmp_tsk_entry, i); 
            my_assert(tmp_tsk_entry != NULL && tmp_tsk_entry->type == TASK_ENTRY_TYPE_EVENT);

            tmp_event = tmp_tsk_entry->ui.event.ev;

            switch(tmp_event->get_type_code())
            {
            case BEGIN_LOAD_QUERY_EVENT:
                tmp_file_id = ((Begin_load_query_log_event*)tmp_event)->file_id;
                break;

            case CREATE_FILE_EVENT:
                tmp_file_id = ((Create_file_log_event*)tmp_event)->file_id;
                break;

            default:
                break;
            }

            if (tmp_file_id == ((Append_block_log_event*)ev)->file_id)
            {
                tsk_entry = binlogex_task_entry_event_simple_new(rec_count, ev, logname, pos, INVALID_THREAD_ID);
                insert_dynamic(&global_load_event_array, (uchar*)tsk_entry);
                break;
            }
        }
        if (i == global_load_event_array.elements)
        {
            delete ev;
            warning("Ignoring Append_block_log since there is no "
                "Begin_load_query event for file_id: %u", ((Append_block_log_event*)ev)->file_id);
        }
        
        break;
    }

    case EXECUTE_LOAD_QUERY_EVENT:
    {
        Execute_load_query_log_event *exlq= (Execute_load_query_log_event*)ev;
        task_entry_t*       tmp_tentry = NULL;
        my_bool             should_skip = FALSE;
        my_bool             find_begin_event;
        Log_event*          tmp_event = NULL;
        uint                tmp_file_id;

        should_skip = shall_skip_database(exlq->db);

        parse_result_init_db(&parse_result, (char*)exlq->db);
        parse_ret = query_parse((char*)exlq->query, &parse_result);
        my_assert(!parse_ret && parse_result.n_tables == 1);

        thread_id = binlogex_get_thread_id_by_name(parse_result.table_arr[0].dbname, parse_result.table_arr[0].tablename);

        /* �����Ƿ���ܶ��load�¼���������� */
        for (i = 0; i < global_load_event_array.elements; ++i)
        {
            tmp_tentry = NULL;
            tmp_event = NULL;
            get_dynamic(&global_load_event_array, (uchar*)&tmp_tentry, i); 
            my_assert(binlogex_task_entry_is_event(tmp_tentry));

            tmp_event = tmp_tentry->ui.event.ev;

            switch(tmp_event->get_type_code())
            {
            case BEGIN_LOAD_QUERY_EVENT:
                tmp_file_id = ((Begin_load_query_log_event*)tmp_event)->file_id;
                break;

            case APPEND_BLOCK_EVENT:
                tmp_file_id = ((Append_block_log_event*)tmp_event)->file_id;
                break;

            default:
                my_assert(0);
                break;
            }

            if (tmp_file_id == exlq->file_id)
            {
                if (tmp_event->get_type_code() == BEGIN_LOAD_QUERY_EVENT )
                    find_begin_event = TRUE;

                if (!should_skip && find_begin_event)
                {
                    /* id use rec_count for simple */
                    binlogex_task_entry_event_set_dst_thread_id(tmp_tentry, thread_id);
                    binlogex_task_entry_add_to_queue(tmp_tentry, thread_id);
                }
                else
                {
                    binlogex_task_entry_destroy(tmp_tentry);
                    delete tmp_event;
                }

                /* �Ӷ�̬������ɾ�� */
                delete_dynamic_element(&global_load_event_array, i);

                i--;
            }
        }
        
        if (!should_skip && find_begin_event)
        {
            tsk_entry = binlogex_task_entry_event_simple_new(rec_count, ev, logname, pos, thread_id);
            binlogex_task_entry_add_to_queue(tsk_entry, thread_id);
        }
        else
        {
            delete ev;
        }

        if (!find_begin_event)
          warning("Ignoring Execute_load_query since there is no "
                  "Begin_load_query event for file_id: %u", exlq->file_id);
        break;
    }

    /* ������һϵ��load�¼���ȡ��������ִ�� */
    case DELETE_FILE_EVENT:
    {
        Delete_file_log_event* dfe = (Delete_file_log_event*)ev;
        uint  tmp_file_id;
        Log_event*  tmp_event;
        task_entry_t* tmp_tsk_entry;

        for (i = 0; i < global_load_event_array.elements; ++i)
        {
            tmp_event = NULL;
            tmp_tsk_entry = NULL;
            get_dynamic(&global_load_event_array, (uchar*)&tmp_tsk_entry, i); 
            my_assert(binlogex_task_entry_is_event(tmp_tsk_entry));

            tmp_event = tmp_tsk_entry->ui.event.ev;

            switch(tmp_event->get_type_code())
            {
            case BEGIN_LOAD_QUERY_EVENT:
                tmp_file_id = ((Begin_load_query_log_event*)tmp_event)->file_id;
                break;

            case CREATE_FILE_EVENT:
                tmp_file_id = ((Create_file_log_event*)tmp_event)->file_id;
                break;

            case APPEND_BLOCK_EVENT:
                tmp_file_id = ((Append_block_log_event*)tmp_event)->file_id;
                break;

            default:
                my_assert(0);
                break;
            }

            if (tmp_file_id == dfe->file_id)
            {
                /* �Ӷ�̬������ɾ�� */
                delete_dynamic_element(&global_load_event_array, i);
                delete tmp_event;
                binlogex_task_entry_destroy(tmp_tsk_entry);

                i--;
            }
        }
        delete ev;

        break;
    }
    /* ROW format */
    /* ע�������������
        Update t1,t2 set t1.c2=10, t2.c2=10 where t1.c1=t2.c1;
        һ�����Ҳ�������table_map��update�¼�
    */
    case TABLE_MAP_EVENT:
    {
        Table_map_log_event *map= ((Table_map_log_event *)ev);
        if (shall_skip_database(map->get_db_name()))
        {
            table_map_ignored.set_table(map->get_table_id(), map);
        }
        else
        {
            table_map_ex_t tmap_ex;

            thread_id = binlogex_get_thread_id_by_name(map->get_db_name(), map->get_table_name());

            tmap_ex.table_id = map->get_table_id();
            tmap_ex.thread_id = thread_id;

#ifndef DBUG_OFF
            my_assert(binlogex_get_thread_id_by_tablemap_id(tmap_ex.thread_id) == -1);
#endif
            /* table map���뵽global_table_map_array,  */
            insert_dynamic(&global_table_map_array, (uchar*)&tmap_ex);

            /* binlog�¼�����Ӧ�߳� */
            tsk_entry = binlogex_task_entry_event_simple_new(rec_count, ev, logname, pos, thread_id);
            binlogex_task_entry_add_to_queue(tsk_entry, thread_id);
        }
        break;
    }
    case WRITE_ROWS_EVENT:
    case DELETE_ROWS_EVENT:
    case UPDATE_ROWS_EVENT:
    case PRE_GA_WRITE_ROWS_EVENT:
    case PRE_GA_DELETE_ROWS_EVENT:
    case PRE_GA_UPDATE_ROWS_EVENT:
    {
        Rows_log_event *e= (Rows_log_event*) ev;
        Table_map_log_event *ignored_map= 
          table_map_ignored.get_table(e->get_table_id());
        bool skip_event= (ignored_map != NULL);

        thread_id = INVALID_THREAD_ID;

        if (skip_event)
        {
            //TODO ������STMT_END_F�߼�
            my_assert(0);
            delete ev;
        }
        else
        {
            thread_id = binlogex_get_thread_id_by_tablemap_id(e->get_table_id());
            my_assert(thread_id != INVALID_THREAD_ID);

            tsk_entry = binlogex_task_entry_event_simple_new(rec_count, ev, logname, pos, thread_id);
            binlogex_task_entry_add_to_queue(tsk_entry, thread_id);
        }

        if (e->get_flags(Rows_log_event::STMT_END_F))
        {
          /* 
            Now is safe to clear ignored map (clear_tables will also
            delete original table map events stored in the map).
          */
          if (table_map_ignored.count() > 0)
            table_map_ignored.clear_tables();

          memset(global_thread_tmap_flag_array, 0, sizeof(uint) * concurrency);

          for (i = 0; i < global_table_map_array.elements; i++)
          {
              table_map_ex_t tmap_ex;

              get_dynamic(&global_table_map_array, (uchar*)&tmap_ex, i);

              if (tmap_ex.thread_id == thread_id)
                  continue;

              /* ÿ���߳�ֻ����һ��STMT_END�������ڴ���Rows_log_event::STMT_END_F���߼�
                ����������Rows_log_event::STMT_END_F,��Ϊֻ�����һ���¼������ñ�ǣ���һ�������ܰ�����������м�binlog */
              if (global_thread_tmap_flag_array[tmap_ex.thread_id] == 0)
              {
                  task_entry_t* se_entry = binlogex_task_entry_stmt_end_new(rec_count);
                  binlogex_task_entry_add_to_queue(se_entry, tmap_ex.thread_id);

                  global_thread_tmap_flag_array[tmap_ex.thread_id] = 1;
              }
          }
          /* ���global_table_map_array */
          global_table_map_array.elements = 0;
        }

        /* skip the event check */
        break;
    }

    case STOP_EVENT:
    case SLAVE_EVENT:
    case ROTATE_EVENT:
    case INCIDENT_EVENT:
    case HEARTBEAT_LOG_EVENT:
        delete ev;
        break;
    default:
        my_assert(0);
    }
  }

  goto end;

err:
  retval= ERROR_STOP;
end:
  rec_count++;
  ///*
  //  Destroy the log_event object. If reading from a remote host,
  //  set the temp_buf to NULL so that memory isn't freed twice.
  //*/
  //if (ev)
  //{
  //  if (remote_opt)
  //    ev->temp_buf= 0;
  //  if (destroy_evt) /* destroy it later if not set (ignored table map) */
  //    delete ev;
  //}
  DBUG_RETURN(retval);
}



