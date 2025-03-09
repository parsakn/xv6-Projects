// Console input and output.
// Input is from the keyboard or serial port.
// Output is written to the screen and serial port.

#include "types.h"
#include "defs.h"
#include "param.h"
#include "traps.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "file.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"
#include "x86.h"


#define LEFT_ARROW 0xE4
#define BLUE_BACKGROUND "\033[44m"

#define RESET "\033[0m"

int backStepCounter = 0 ; 
int ctrlc_flag = 0;
int upper_copy_index = 0;
int downer_copy_index = 0;
char copy_buffer[100];
int copybufLen = 0 ;

static void consputc(int);

static int panicked = 0;

static struct {
  struct spinlock lock;
  int locking;
} cons;

static void
printint(int xx, int base, int sign)
{
  static char digits[] = "0123456789abcdef";
  char buf[16];
  int i;
  uint x;

  if(sign && (sign = xx < 0))
    x = -xx;
  else
    x = xx;

  i = 0;
  do{
    buf[i++] = digits[x % base];
  }while((x /= base) != 0);

  if(sign)
    buf[i++] = '-';

  while(--i >= 0)
    consputc(buf[i]);
}
//PAGEBREAK: 50

// Print to the console. only understands %d, %x, %p, %s.
void
cprintf(char *fmt, ...)
{
  int i, c, locking;
  uint *argp;
  char *s;

  locking = cons.locking;
  if(locking)
    acquire(&cons.lock);

  if (fmt == 0)
    panic("null fmt");

  argp = (uint*)(void*)(&fmt + 1);
  for(i = 0; (c = fmt[i] & 0xff) != 0; i++){
    if(c != '%'){
      consputc(c);
      continue;
    }
    c = fmt[++i] & 0xff;
    if(c == 0)
      break;
    switch(c){
    case 'd':
      printint(*argp++, 10, 1);
      break;
    case 'x':
    case 'p':
      printint(*argp++, 16, 0);
      break;
    case 's':
      if((s = (char*)*argp++) == 0)
        s = "(null)";
      for(; *s; s++)
        consputc(*s);
      break;
    case '%':
      consputc('%');
      break;
    default:
      // Print unknown % sequence to draw attention.
      consputc('%');
      consputc(c);
      break;
    }
  }

  if(locking)
    release(&cons.lock);
}

void
panic(char *s)
{
  int i;
  uint pcs[10];

  cli();
  cons.locking = 0;
  // use lapiccpunum so that we can call panic from mycpu()
  cprintf("lapicid %d: panic: ", lapicid());
  cprintf(s);
  cprintf("\n");
  getcallerpcs(&s, pcs);
  for(i=0; i<10; i++)
    cprintf(" %p", pcs[i]);
  panicked = 1; // freeze other CPU
  for(;;)
    ;
}

//PAGEBREAK: 50
#define BACKSPACE 0x100
#define CRTPORT 0x3d4
static ushort *crt = (ushort*)P2V(0xb8000);  // CGA memory

static void
cgaputc(int c)
{
  int pos;

  // Cursor position: col + 80*row.
  outb(CRTPORT, 14);
  pos = inb(CRTPORT+1) << 8;
  outb(CRTPORT, 15);
  pos |= inb(CRTPORT+1);

  if(c == '\n')
    pos += 80 - pos%80;
  else if(c == BACKSPACE){
    if(pos > 0) --pos;
  } else
    crt[pos++] = (c&0xff) | 0x0700;  // black on white

  if(pos < 0 || pos > 25*80)
    panic("pos under/overflow");

  if((pos/80) >= 24){  // Scroll up.
    memmove(crt, crt+80, sizeof(crt[0])*23*80);
    pos -= 80;
    memset(crt+pos, 0, sizeof(crt[0])*(24*80 - pos));
  }

  outb(CRTPORT, 14);
  outb(CRTPORT+1, pos>>8);
  outb(CRTPORT, 15);
  outb(CRTPORT+1, pos);
  crt[pos] = ' ' | 0x0700;
}


  
  






void
consputc(int c)
{
  if(panicked){
    cli();
    for(;;)
      ;
  }

  if(c == BACKSPACE){
    uartputc('\b'); uartputc(' '); uartputc('\b');
  } else
    uartputc(c);
  cgaputc(c);
}

#define INPUT_BUF 128
struct {
  char buf[INPUT_BUF];
  uint r;  // Read index
  uint w;  // Write index
  uint e;  // Edit index
} input;

void
consputs(const char* s){
  for(int i = 0; i < INPUT_BUF && (s[i]); ++i){
    input.buf[input.e++ % INPUT_BUF] = s[i];
    consputc(s[i]);
  }
}
void
consputsblue(const char* s){
  // Print the blue background start sequence
  for (int i = 0; BLUE_BACKGROUND[i]; i++) {
    input.buf[input.e++ % INPUT_BUF] = BLUE_BACKGROUND[i];
    consputc(BLUE_BACKGROUND[i]);
  }

  // Print the actual string
  for(int i = 0; i < INPUT_BUF && (s[i]); ++i){
    input.buf[input.e++ % INPUT_BUF] = s[i];
    consputc(s[i]);
  }

  // Print the reset sequence
  for (int i = 0; RESET[i]; i++) {
    input.buf[input.e++ % INPUT_BUF] = RESET[i];
    consputc(RESET[i]);
  }
}

void
consclear(){
  while(input.e != input.w &&
        input.buf[(input.e-1) % INPUT_BUF] != '\n'){
    input.e--;
    consputc(BACKSPACE);
  }
}

#define HIST_SIZE 10
struct {
  uint queue_idx;
  char cmd_buf[HIST_SIZE][INPUT_BUF];
  uint last_used_idx;
  uint last_arrow_idx;

  int is_suggestion_used;
  char original_cmd[INPUT_BUF];
  uint original_cmd_size;

  int total_count;
  int last_arrow_total;
} hist;

static int
get_suggestion(const char* cmd, uint cmd_size)
{
  for(int i = 0; i < HIST_SIZE; ++i){
    int idx = (i + hist.last_used_idx) % HIST_SIZE;
    if(strncmp(cmd, hist.cmd_buf[idx], cmd_size) == 0){
      return idx;
    }
  }
  return -1;
}

static void
suggest_cmd()
{
  if(!hist.is_suggestion_used){
    hist.original_cmd_size = input.e - input.w;
    memmove(hist.original_cmd, input.buf + input.w, hist.original_cmd_size);
  }
  int suggested_cmd = get_suggestion(hist.original_cmd, hist.original_cmd_size);
  if(suggested_cmd >= 0){
    hist.is_suggestion_used = 1;
    hist.last_used_idx = suggested_cmd + 1;
    consclear();
    consputs(hist.cmd_buf[suggested_cmd]);
  }
  else // beep
    consputc('\a');
}

static void
last5cmd()
{

      int indexes[5] = {0,0,0,0,0};
    int mod = hist.total_count %10;
    for(int i = 0 ; i < 5;i++){
      if(mod == 0){
        mod = 10;
      }
      indexes[i] = mod - 1;
      mod--;
    }

    int temp = 0;
    consputs("Last 5: ");

    if(hist.total_count > 10){
      for(int j = 0 ; j < 5 ; j++){
          consputs(hist.cmd_buf[indexes[j]]);
          consputs("  ");      
          }

    }else{
      for(int i = 0; i < HIST_SIZE; i++){
          if (hist.cmd_buf[i][0] != '\0'){
                temp++;
                }
      }
      int lowside = 0;
      if (temp - 5 < 0)
      {
        lowside = 0;
      }else{
        lowside = temp - 5;
      }
      
      
      for(int i = temp-1; i >= lowside; i--){
        
          consputs(hist.cmd_buf[i]);
          consputs("  ");
        
      }
    }
}


static void
push_current_hist()
{
  if(input.e - input.w == 1)
    return;
  memset(hist.cmd_buf[hist.queue_idx], 0, INPUT_BUF);
  memmove(hist.cmd_buf[hist.queue_idx],
          input.buf + input.w,
          input.e - input.w - 1);
  hist.queue_idx = (hist.queue_idx + 1) % HIST_SIZE;
  hist.last_arrow_idx = hist.queue_idx;
  hist.total_count++;
  hist.last_arrow_total = hist.total_count;
  hist.is_suggestion_used = 0;
  hist.last_used_idx = 0;
  memset(hist.original_cmd, 0, INPUT_BUF);
}

void
revstr(char* src, uint len)
{
  int i = 0, j = len - 1;
  while (i < j) {
    char tmp = src[i];
    src[i] = src[j];
    src[j] = tmp;
    i++;
    j--;
  }
}


static void
hist_up()
{
  if(hist.last_arrow_total > 0 &&
     hist.last_arrow_total > hist.total_count - HIST_SIZE){
    hist.last_arrow_total--;
    hist.last_arrow_idx = (hist.last_arrow_idx - 1 + HIST_SIZE) % HIST_SIZE;
    consclear();
    consputs(hist.cmd_buf[hist.last_arrow_idx]);
  }
  else // beep
    consputc('\a');
}

static void
hist_down()
{
  if(hist.last_arrow_total < hist.total_count){
    hist.last_arrow_total++;
    hist.last_arrow_idx = (hist.last_arrow_idx + 1) % HIST_SIZE;
    consclear();
    consputs(hist.cmd_buf[hist.last_arrow_idx]);
  }
  else // beep
    consputc('\a');
}

#define C(x)  ((x)-'@')  // Control-x
#define ARROW_UP 65
#define ARROW_DOWN 66




int charsInBuffer() {
    int out = 0;
    for(int i = 0; i < INPUT_BUF ; i++){
      if(input.buf[i] != '\0'){
        out++;
      }
    }
    return out;
}

void paste_action(){
  for (int i = 0; i < copybufLen; i++)
  {
    consputc(copy_buffer[i]);
  }
    consputs("\n");

}


void copy_action(){
  ctrlc_flag++;
  if (ctrlc_flag == 1)
  {
      upper_copy_index = backStepCounter;
  }
  if (ctrlc_flag == 2)
  {
    downer_copy_index = backStepCounter;
    backStepCounter = 0;
    ctrlc_flag = 0;
    int buflen = charsInBuffer();
    int buf_index = 0;
    for (int i = buflen - downer_copy_index ; i < buflen - upper_copy_index ; i++)
    {

        copy_buffer[buf_index] = input.buf[i];
        buf_index++;
    }
    copybufLen = buf_index;
    
  }
  
}





void
consoleintr(int (*getc)(void))
{
  int c, doprocdump = 0;

  acquire(&cons.lock);
  while((c = getc()) >= 0){
    switch(c){
    case C('P'):  // Process listing.
      // procdump() locks cons.lock indirectly; invoke later
      doprocdump = 1;
      break;
    case C('U'):  // Kill line.
      consclear();
      break;
    case C('H'): case '\x7f':  // Backspace
      if(input.e != input.w){
        input.e--;
        consputc(BACKSPACE);
        hist.is_suggestion_used = 0;
      }
      break;
    case C('R'): 
      last5cmd();
      break;
    case C('C'):
      copy_action();
      break;
    case C('V'):
      paste_action();
      break;
    case '\t': // Suggest command from history.
      suggest_cmd();
      break;
    case LEFT_ARROW:
      if(input.e > input.w){
          backStepCounter ++;
    }
      break;
    case 27: // Escape sequence for arrow history.
      if((c = getc()) == 91){
        if((c = getc()) == ARROW_UP){
          hist_up();
          break;
        }
        else if (c == ARROW_DOWN){
          hist_down();
          break;
        }
        else{
          input.buf[input.e++ % INPUT_BUF] = 27;
          consputc(27);
          input.buf[input.e++ % INPUT_BUF] = 91;
          consputc(91);
        }
      }
      else{
        input.buf[input.e++ % INPUT_BUF] = 27;
        consputc(27);
      }
    default:
      if(c != 0 && input.e-input.r < INPUT_BUF){
        c = (c == '\r') ? '\n' : c;
        input.buf[input.e++ % INPUT_BUF] = c;
        consputc(c);
        if(c == '\n' || c == C('D') || input.e == input.r+INPUT_BUF){
          push_current_hist();
          input.w = input.e;
          wakeup(&input.r);
        }
      }
      break;
    }
  }
  release(&cons.lock);
  if(doprocdump) {
    procdump();  // now call procdump() wo. cons.lock held
  }
}

int
consoleread(struct inode *ip, char *dst, int n)
{
  uint target;
  int c;

  iunlock(ip);
  target = n;
  acquire(&cons.lock);
  while(n > 0){
    while(input.r == input.w){
      if(myproc()->killed){
        release(&cons.lock);
        ilock(ip);
        return -1;
      }
      sleep(&input.r, &cons.lock);
    }
    c = input.buf[input.r++ % INPUT_BUF];
    if(c == C('D')){  // EOF
      if(n < target){
        // Save ^D for next time, to make sure
        // caller gets a 0-byte result.
        input.r--;
      }
      break;
    }
    *dst++ = c;
    --n;
    if(c == '\n')
      break;
  }
  release(&cons.lock);
  ilock(ip);

  return target - n;
}

int
consolewrite(struct inode *ip, char *buf, int n)
{
  int i;

  iunlock(ip);
  acquire(&cons.lock);
  for(i = 0; i < n; i++)
    consputc(buf[i] & 0xff);
  release(&cons.lock);
  ilock(ip);

  return n;
}

void
consoleinit(void)
{
  initlock(&cons.lock, "console");

  devsw[CONSOLE].write = consolewrite;
  devsw[CONSOLE].read = consoleread;
  cons.locking = 1;

  ioapicenable(IRQ_KBD, 0);
}
