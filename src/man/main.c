#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <getopt.h>
#include <inttypes.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <notcurses/notcurses.h>
#include "builddef.h"

static void
usage(const char* argv0, FILE* o){
  fprintf(o, "usage: %s [ -hV ] files\n", argv0);
  fprintf(o, " -h: print help and return success\n");
  fprintf(o, " -v: print version and return success\n");
}

static int
parse_args(int argc, char** argv){
  const char* argv0 = *argv;
  int longindex;
  int c;
  struct option longopts[] = {
    { .name = "help", .has_arg = 0, .flag = NULL, .val = 'h', },
    { .name = NULL, .has_arg = 0, .flag = NULL, .val = 0, }
  };
  while((c = getopt_long(argc, argv, "hV", longopts, &longindex)) != -1){
    switch(c){
      case 'h': usage(argv0, stdout);
                exit(EXIT_SUCCESS);
                break;
      case 'V': fprintf(stderr, "ncman version %s\n", notcurses_version());
                exit(EXIT_SUCCESS);
                break;
      default: usage(argv0, stderr);
               return -1;
               break;
    }
  }
  if(argv[optind] == NULL){
    usage(argv0, stderr);
    return -1;
  }
  return optind;
}

#ifdef USE_DEFLATE // libdeflate implementation
#include <libdeflate.h>
// assume that |buf| is |*len| bytes of deflated data, and try to inflate
// it. if successful, the inflated map will be returned. either way, the
// input map will be unmapped (we take ownership). |*len| will be updated
// if an inflated map is successfully returned.
static unsigned char*
map_gzipped_data(unsigned char* buf, size_t* len, unsigned char* ubuf, uint32_t ulen){
  struct libdeflate_decompressor* inflate = libdeflate_alloc_decompressor();
  size_t outbytes;
  enum libdeflate_result r;
  r = libdeflate_gzip_decompress(inflate, buf, *len, ubuf, ulen, &outbytes);
  munmap(buf, *len);
  libdeflate_free_decompressor(inflate);
  if(r != LIBDEFLATE_SUCCESS){
    return NULL;
  }
  *len = ulen;
  return ubuf;
}
#else // libz implementation
#error libz not yet implemented, need libdeflate
static unsigned char*
map_gzipped_data(unsigned char* buf, size_t* len, unsigned char* ubuf, uint32_t ulen){
  munmap(buf, *len);
  (void)ulen; // FIXME
  return NULL;
}
#endif

static unsigned char*
map_troff_data(int fd, size_t* len){
  struct stat sbuf;
  if(fstat(fd, &sbuf)){
    return NULL;
  }
  // gzip has a 10-byte mandatory header and an 8-byte mandatory footer
  if(sbuf.st_size < 18){
    return NULL;
  }
  *len = sbuf.st_size;
  unsigned char* buf = mmap(NULL, *len, PROT_READ,
                            MAP_PRIVATE | MAP_POPULATE, fd, 0);
  if(buf == MAP_FAILED){
    return NULL;
  }
  if(buf[0] == 0x1f && buf[1] == 0x8b && buf[2] == 0x08){
    // the last four bytes have the uncompressed length
    uint32_t ulen;
    memcpy(&ulen, buf + *len - 4, 4);
    size_t pgsize = 4096; // FIXME
    void* ubuf = mmap(NULL, (ulen + pgsize - 1) / pgsize * pgsize,
                      PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if(ubuf == MAP_FAILED){
      munmap(buf, *len);
      return NULL;
    }
    if(map_gzipped_data(buf, len, ubuf, ulen) == NULL){
      munmap(ubuf, ulen);
      return NULL;
    }
    return ubuf;
  }
  return buf;
}

// find the man page, and inflate it if deflated
static unsigned char*
get_troff_data(const char *arg, size_t* len){
  // FIXME we'll want to use the mandb. for now, require a full path.
  int fd = open(arg, O_RDONLY | O_CLOEXEC);
  if(fd < 0){
    return NULL;
  }
  unsigned char* buf = map_troff_data(fd, len);
  close(fd);
  return buf;
}

// calculate the number of rows necessary to display the troff data,
// assuming the specified width |dimx|.
static int
troff_height(unsigned dimx, const unsigned char* map, size_t mlen){
  // FIXME for now we assume infinitely wide lines
  int lines = 0;
  enum {
    LINE_UNKNOWN,
    LINE_COMMENT,
    LINE_B, LINE_BI, LINE_BR, LINE_I, LINE_IB, LINE_IR,
    LINE_RB, LINE_RI, LINE_SB, LINE_SM,
    LINE_EE, LINE_EX, LINE_RE, LINE_RS,
    LINE_SH, LINE_SS, LINE_TH,
    LINE_IP, LINE_LP, LINE_P, LINE_PP,
    LINE_TP, LINE_TQ,
    LINE_ME, LINE_MT, LINE_UE, LINE_UR,
    LINE_OP, LINE_SY, LINE_YS,
  } linetype = LINE_UNKNOWN;
  const unsigned char comment[] = ".\\\""; // dot slash quot
  const unsigned char* comiter = comment;
  for(size_t off = 0 ; off < mlen ; ++off){
    if(map[off] == '\n'){
      if(linetype != LINE_UNKNOWN && linetype != LINE_COMMENT){
        ++lines;
      }
      linetype = LINE_UNKNOWN;
      comiter = comment;
    }else if(linetype == LINE_UNKNOWN){
      if(comiter && map[off] == *comiter){
        if(*++comiter == '\0'){
          linetype = LINE_COMMENT;
        }else{
          comiter = NULL;
        }
      }
    }
  }
  if(linetype != LINE_UNKNOWN && linetype != LINE_COMMENT){
    ++lines;
  }
  return lines;
}

// we create a plane sized appropriately for the troff data. all we do
// after that is move the plane up and down.
static struct ncplane*
render_troff(struct notcurses* nc, const unsigned char* map, size_t mlen){
  unsigned dimy, dimx;
  struct ncplane* stdn = notcurses_stddim_yx(nc, &dimy, &dimx);
  int rows = troff_height(dimx, map, mlen);
  struct ncplane_options popts = {
    .rows = rows,
    .cols = dimx,
  };
  struct ncplane* pman = ncplane_create(stdn, &popts);
  // FIXME draw it
  return pman;
}

static int
manloop(struct notcurses* nc, const char* arg){
  size_t len;
  unsigned char* buf = get_troff_data(arg, &len);
  if(buf == NULL){
    return -1;
  }
  struct ncplane* page = render_troff(nc, buf, len);
  uint32_t key;
  do{
    if(notcurses_render(nc)){
      munmap(buf, len);
      return -1;
    }
    ncinput ni;
    key = notcurses_get(nc, NULL, &ni);
    switch(key){
      case 'q':
        munmap(buf, len);
        return 0;
    }
  }while(key != (uint32_t)-1);
  munmap(buf, len);
  return -1;
}

static int
ncman(struct notcurses* nc, const char* arg){
  // FIXME usage bar at bottom
  return manloop(nc, arg);
}

int main(int argc, char** argv){
  int nonopt = parse_args(argc, argv);
  if(nonopt <= 0){
    return EXIT_FAILURE;
  }
  struct notcurses_options nopts = {
    .flags = NCOPTION_NO_ALTERNATE_SCREEN,
  };
  struct notcurses* nc = notcurses_core_init(&nopts, NULL);
  if(nc == NULL){
    return EXIT_FAILURE;
  }
  bool success;
  for(int i = 0 ; i < argc - nonopt ; ++i){
    success = false;
    if(ncman(nc, argv[nonopt + i])){
      break;
    }
    success = true;
  }
  return notcurses_stop(nc) || !success ? EXIT_FAILURE : EXIT_SUCCESS;
}