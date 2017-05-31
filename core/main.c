#include <signal.h>
#include <getopt.h>
#include <string.h>
#include "err_msg.h"
#include "defs.h"
#include "map.h"
#include "vm.h"
#include "lang.h"
#include "absyn.h"
#include "emit.h"
#include "compile.h"
#include "udp.h"
#include "driver.h"
#include "shreduler.h"
#include "instr.h"
#include "code_private.h"

volatile m_bool signaled = 0;
static VM* vm;

static void sig(int unused) {
  vm->is_running = 0;
  vm->wakeup();
  signaled = 1;
}

m_bool compile(VM* vm, const m_str filename) {
  m_bool ret = 1;
  VM_Shred shred;
  VM_Code  code;
  Ast      ast;
  Vector args = NULL;
  m_str _name, name, d = strdup(filename);
  _name = strsep(&d, ":");
  if(d)
    args = new_vector();
  while(d)
    vector_append(args, (vtype)S_name(insert_symbol(strsep(&d, ":"))));
  free(d);
  name = realpath(_name, NULL);
  free(_name);
  if(!name) {
    err_msg(COMPILE_, 0, "error while opening file '%s'", filename);
    return -1;
  }
#ifdef DEBUG_COMPILE
  debug_msg("parser", "get full path ok %s", name);
#endif
  if(!(ast = parse(name))) {
    ret = -1;
    goto clean;
  }
#ifdef DEBUG_COMPILE
  debug_msg("lexer", "Ast of '%s' ok", name);
#endif
  CHECK_BB(type_engine_check_prog(vm->env, ast, name))
#ifdef DEBUG_COMPILE
  debug_msg("lexer", "type check  of '%s' ok", name);
#endif
  CHECK_BB(emit_ast(vm->emit, ast, name))
#ifdef DEBUG_COMPILE
  debug_msg("lexer", "emit   of '%s' ok", name);
#endif
  add_instr(vm->emit, EOC);
  vm->emit->code->name = strdup(name);
  vm->emit->code->filename = strdup(name);
  code = emit_to_code(vm->emit);
  free_ast(ast);
  shred = new_VM_Shred(code);
  shred->args = args;
  shred->me = new_Shred(vm, shred);
  vm_add_shred(vm, shred);
clean:
  free(name);
  return ret;
}

static const struct option long_option[] = {
  { "add",      0, NULL, '+' },
  { "rem",      0, NULL, 'z' },
  { "pludir",   0, NULL, 'P' },
  { "quit",     0, NULL, 'q' },
  { "driver",   1, NULL, 'd' },
  { "card",     1, NULL, 'c' },
  { "backend",  1, NULL, 'b' },
  { "sr",       1, NULL, 's' },
  { "name",     1, NULL, 'n' },
  { "raw",      0, NULL, 'r' },
  { "host",     1, NULL, 'h' },
  { "port",     1, NULL, 'p' },
  { "rate",     1, NULL, 'r' },
  { "alone",    1, NULL, 'a' },
  { "in",       1, NULL, 'i' },
  { "out",      1, NULL, 'o' },
  { "bufsize",  1, NULL, 'b' },
  { "bufnum",   1, NULL, 'n' },
  { "loop",     1, NULL, 'l' },
  { "format",   1, NULL, 'l' },
  { "help",     0, NULL, '?' },
  { "config",   0, NULL, 'C' },
  /*  { "status"  , 0, NULL, '%' },*/
  { NULL,       0, NULL, 0   }
};

static void usage() {
  printf("usage: Gwion <options>\n");
  printf("\toption can be any of:\n");
  printf("GLOBAL options:  <argument>  : description\n");
  printf("\t--help,   -?\t             : this help\n");
  printf("\t--config, -C\t             : this help\n");
  printf("VM     options:\n");
  printf("\t--add,    -+\t <file>      : add file\n");
  printf("\t--rem,    --\t <shred id>  : remove shred\n");
  printf("\t--plugdir,-P\t <directory> : add a plugin directory\n");
  printf("\t--quit    -q\t             : quit the vm\n");
  printf("UDP    options:\n");
  printf("\t--host    -h\t  <string>   : set host\n");
  printf("\t--port    -p\t  <number>   : set port\n");
  printf("\t--loop    -l\t  <0 or 1>   : loop state (0 or 1)\n");
  printf("\t--alone   -a\t             : standalone mode. (no udp)\n");
  printf("DRIVER options:\n");
  printf("\t--driver  -d\t  <string>   : set the driver (one of: alsa jack soundio portaudio file dummy silent raw)\n");
  printf("\t--sr      -s\t  <number>   : set samplerate\n");
  printf("\t--bufnum  -n\t  <number>   : set number of buffers\n");
  printf("\t--bufsize -b\t  <number>   : set size   of buffers\n");
  printf("\t--chan    -g\t  <number>   : (global) channel number\n");
  printf("\t--in      -i\t  <number>   : number of  input channel\n");
  printf("\t--out     -o\t  <number>   : number of output channel\n");
  printf("\t--card    -c\t  <string>   : card identifier or output file (depending on driver)\n");
  printf("\t--raw     -r\t  <0 or 1>   : enable raw mode (file and soundio only)\n");
  printf("\t--format  -f\t  <string>   : soundio format (one of: S8 U8 S16 U16 S24 U24 S32 U32 F32 F64)\n");
  printf("\t--backend -e\t  <string>   : soundio backend (one of: jack pulse alsa core wasapi dummy)\n");
}

int main(int argc, char** argv) {
  Driver* d = NULL;
  int i, index;
  Vector add  = new_vector();
  Vector rem  = new_vector();
  Vector ref  = add;

  Vector plug_dirs = new_vector();

  int do_quit = 0;
  m_bool udp = 1;
  int port = 8888;
  char* hostname = "localhost";
  int loop = 0;
  DriverInfo di;
  di.func = D_FUNC;
  di.in  = 2;
  di.out = 2;
  di.chan = 2;
  di.sr = 48000;
  di.bufsize = 256;
  di.bufnum = 3;
  di.card = "default:CARD=CODEC";
  di.raw = 0;

  vector_append(plug_dirs, (vtype)GWION_ADD_DIR);

  while((i = getopt_long(argc, argv, "?Cqh:p:i:o:n:b:e:s:d:al:g:-:rc:f:P: ", long_option, &index)) != -1) {
    switch(i) {
    case '?':
      usage();
      exit(0);
    case 'C':
      printf("CFLAGS: %s\nLDFLAGS: %s\n", CFLAGS, LDFLAGS);
      do_quit     = 1;
      break;
    case 'q':
      do_quit     = 1;
      break;
    case 'c':
      di.card     = optarg;
      break;
    case 'h':
      hostname    = optarg;
      break;
    case 'p':
      port        = atoi(optarg);
      break;
    case 'n':
      di.bufnum    = atoi(optarg);
      break;
    case 'b':
      di.bufsize    = atoi(optarg);
      break;
    case 'g':
      di.chan       = atoi(optarg);
      di.in       = atoi(optarg);
      di.out       = atoi(optarg);
      break;
    case 'i':
      di.in       = atoi(optarg);
      break;
    case 'o':
      di.out      = atoi(optarg);
      break;
    case 's':
      di.sr      = atoi(optarg);
      break;
    case 'l':
      loop        = atoi(optarg);
      if(loop == 0) loop = -1;
      break;
    case 'd':
      select_driver(&di, optarg);
      break;
    case 'f':
      select_format(&di, optarg);
      break;
    case 'e':
      select_backend(&di, optarg);
      break;
    case 'r':
      di.raw = 1;
      break;
    case 'a':
      udp = 0;
      break;
    case 'P':
      vector_append(plug_dirs, (vtype)optarg);
      break;
    default:
      return 1;
    }
  }
  if(optind < argc) {
    while(optind < argc) {
      m_str str = argv[optind++];
      if(!strcmp(str, "-")) {
        ref = rem;
        str = argv[optind++];
      } else if(!strcmp(str, "+")) {
        ref = add;
        str = argv[optind++];
      }
      vector_append(ref, (vtype)str);
    }
  }
  pthread_t udp_thread = 0;
  if(udp) {
    if(server_init(hostname, port) == -1) {
      if(do_quit)
        Send("quit", 1);
      if(loop > 0)
        Send("loop 1", 1);
      else if(loop < 0)
        Send("loop 0", 1);
      for(i = 0; i < vector_size(rem); i++) {
        m_str file = (m_str)vector_at(rem, i);
        m_uint size = strlen(file) + 3;
        char name[size];
        memset(name, 0, size);
        strcpy(name, "- ");
        strcat(name, file);
        Send(name, 1);
      }
      for(i = 0; i < vector_size(add); i++) {
        m_str file = (m_str)vector_at(add, i);
        m_uint size = strlen(file) + 3;
        char name[size];
        memset(name, 0, size);
        strcpy(name, "+ ");
        strcat(name, file);
        Send(name, 1);
      }
      free_vector(add);
      free_vector(rem);
      exit(0);
    }
  }
  if(do_quit)
    goto clean;
  signal(SIGINT, sig);
  signal(SIGTERM, sig);
  scan_map = new_Map();
  if(!loop)
    loop = -1;
  if(!(vm = new_VM(loop)))
    goto clean;
  if(!(vm->bbq = new_BBQ(vm, &di, &d)))
    goto clean;
  if(!(vm->env = type_engine_init(vm, plug_dirs)))
    goto clean;
  if(!(vm->emit = new_emitter(vm->env)))
    goto clean;
  srand(time(NULL));

  for(i = 0; i < vector_size(add); i++)
    compile(vm, (m_str)vector_at(add, i));

  vm->is_running = 1;
  if(udp) {
    pthread_create(&udp_thread, NULL, server_thread, vm);
#ifndef __linux__
    pthread_detach(udp_thread);
#endif
  }
  d->run(vm, &di);
  if(udp)
    server_destroy(udp_thread);
clean:
  free_vector(plug_dirs);
  free_vector(add);
  free_vector(rem);
  if(d)
    free_driver(d, vm);
  if(scan_map)
    free_Map(scan_map);

  set_nspc_vm(vm);
#ifndef __linux__
  sleep(1);
#endif
  if(vm && !signaled)
    free_VM(vm);
  free_Symbols();
  return 0;
}
