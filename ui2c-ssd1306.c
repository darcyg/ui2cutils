#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <ctype.h>
#include <unistd.h>
#include <errno.h>

#include <fcntl.h>
#include <linux/i2c-dev.h>

#include <malloc.h>
#include <strings.h>
#include <signal.h>
#include <png.h>

/*
 * TODO: define and use a ssd1306_t context as follows:
 * typedef struct {
 *   int file;
 *   size_t width;
 *   size_t heigh;
 *   uint8_t buffer[0];
 * } ssd1306_t;
 *
 * TODO: since malloc is involved, use valgrind to check memory leaks.
 * TODO: very high sys cpu usage. find a way to optimize.
 * TODO: fold consecutive calls into loops with array
 * TODO: try SMBus block write and quick write
 */

/* Signal handling. */
volatile bool stop;

static void sigint_handler(int sig) {
  stop = true;

  /* Unregister myself. */
  struct sigaction sia;

  bzero(&sia, sizeof(sia));
  sia.sa_handler = SIG_DFL;

  if (sigaction(SIGINT, &sia, NULL) < 0) {
    perror("sigaction(SIGINT, SIG_DFL)");
  }
}

/* I2C functions */

#define SSD1306_DEVAD_A   (0x3c)
#define SSD1306_DEVAD_B   (0x3d)
#define SSD1306_CTRL_DATA (1<<6)
#define SSD1306_CTRL_CMD  (0<<6)
#define SSD1306_CTRL_CONT (1<<7)

/* This is how your image displays on the screen, with parameters in this program. */
int dump_bmp(uint8_t data[], size_t len) {
  size_t x, y;

  if ((NULL == data) || ((len % 8) != 1)) {
    fprintf(stdout, "Invalid argument while calling dump_bmp().\n");
    return -EINVAL;
  }

  fprintf(stdout, "HDR = 0x%02x\n", data[0]);
  for (y = 0; y < 64; y ++) {
    for (x = 0; x < 128; x ++) {
      fputc((data[(y / 8 * 128) + x + 1] & (1 << (y % 8))) ? '@' : ' ', stdout);
    }
    if (x % 128 == 0) {
      fputc('\n', stdout);
    }
  }

  fputc('\n', stdout);
  fflush(stdout);
  return 0;
}

int i2c_open(int bus) {
  const int fn_len = 20;
  char fn[fn_len];
  int res, file;

  if (bus < 0) {
    return -EINVAL;
  }

  /* Open i2c-dev file */
  snprintf(fn, fn_len, "/dev/i2c-%d", bus);
  if ((file = open(fn, O_RDWR)) < 0) {
    perror("open() failed (make sure i2c_dev is loaded and you have the permission)");
    return file;
  }

  /* Query functions */
  unsigned long funcs;
  if ((res = ioctl(file, I2C_FUNCS, &funcs)) < 0) {
    perror("ioctl() I2C_FUNCS failed");
    return res;
  }
  fprintf(stdout, "Device: %s (", fn);
  if (funcs & I2C_FUNC_I2C) {
    fputs("I2C_FUNC_I2C ", stdout);
  }
  if (funcs & I2C_FUNC_SMBUS_BYTE) {
    fputs("I2C_FUNC_SMBUS_BYTE ", stdout);
  }
  fputs("\b)\n", stdout);
  fflush(stdout);

  return file;
}

int i2c_select(int file, int addr) {
  /* addr in [0x00, 0x7f] */
  int res;

  if ((res = ioctl(file, I2C_SLAVE, addr)) < 0) {
    perror("ioctl() I2C_SLAVE failed");
  }

  return res;
}

/******************************************************************************
 * Device is mostly write-only.
 * Frame format: address control data
 * Control: CONT D/C 0 0 0 0 0 0
 * Data   : Encapsulated 8-bit command, parameter or data
 * Repeat with CONT set until all command and data are sent.
 *****************************************************************************/

int i2c_write_cmd_1b(int file, uint8_t cmd) {
  int res;
  uint8_t buf[2] = {SSD1306_CTRL_CMD, cmd};

  if ((res = write(file, buf, 2)) < 0) {
    perror("write() command failed");
    return res;
  }

  return 0;
}

#define SSD1306_CONT_DATA_HDR (0x40)
/* To avoid copying, caller should prepare the header. */
int i2c_write_data(int file, uint8_t data[], size_t len) {
  int res;

  if (NULL == data) {
    return -EINVAL;
  }
  if (SSD1306_CONT_DATA_HDR != data[0]) {
    return -EINVAL;
  }

  if ((res = write(file, data, len)) < 0) {
    perror("write() data failed");
    return res;
  }

  return 0;
}

int i2c_read_byte(int file, uint8_t *data) {
  if (NULL == data) {
    return -EFAULT;
  }

  int res;

  if ((res = read(file, data, 1)) < 0) {
    perror("read() data failed");
    return res;
  }

  return 0;
}

/* Device functions */
/* Due to the complicated and variable command structure, use functions instead of macros. */

int ssd1306_set_contrast(int file, uint8_t contrast) {
  int res;

  if ((res = i2c_write_cmd_1b(file, 0x80)) < 0 ) {
    return res;
  }
  if ((res = i2c_write_cmd_1b(file, contrast)) < 0) {
    return res;
  }

  return 0;
}

int ssd1306_reset_contrast(int file) {
  return ssd1306_set_contrast(file, 0x7f);
}

int ssd1306_set_display_test(int file, bool enable) {
  return i2c_write_cmd_1b(file, enable ? 0xa5 : 0xa4);
}

int ssd1306_reset_display_test(int file) {
  return ssd1306_set_display_test(file, false);
}

int ssd1306_set_inverse(int file, bool enable) {
  return i2c_write_cmd_1b(file, enable ? 0xa7 : 0xa6);
}

int ssd1306_reset_inverse(int file) {
  return ssd1306_set_inverse(file, false);
}

int ssd1306_set_power(int file, bool enable) {
  return i2c_write_cmd_1b(file, enable ? 0xaf : 0xae);
}

int ssd1306_reset_power(int file) {
  return ssd1306_set_power(file, false);
}

int ssd1306_interval_to_param(int interval, uint8_t *param) {
  if (NULL == param) {
    return -EFAULT;
  }

  switch (interval) {
    case   2: {
      *param = 0x07;
      return 0;
    }
    case   3: {
      *param = 0x04;
      return 0;
    }
    case   4: {
      *param = 0x05;
      return 0;
    }
    case   5: {
      *param = 0x00;
      return 0;
    }
    case  25: {
      *param = 0x06;
      return 0;
    }
    case  64: {
      *param = 0x01;
      return 0;
    }
    case 128: {
      *param = 0x02;
      return 0;
    }
    case 256: {
      *param = 0x03;
      return 0;
    }

    default: {
      return -EINVAL;
    }
  }
}

int ssd1306_setup_horiz_scroll(int file, bool left, uint8_t start_page, uint8_t end_page, int interval) {
  int res;

  if ((start_page > 0x07) || (end_page > 0x07) || (start_page > end_page)) {
    return -EINVAL;
  }

  uint8_t interval_param;
  if ((res = ssd1306_interval_to_param(interval, &interval_param)) < 0) {
    return res;
  }

  if ((res = i2c_write_cmd_1b(file, left ? 0x27 : 0x26)) < 0 ) {
    return res;
  }
  if ((res = i2c_write_cmd_1b(file, 0x00)) < 0 ) {
    return res;
  }
  if ((res = i2c_write_cmd_1b(file, start_page)) < 0 ) {
    return res;
  }
  if ((res = i2c_write_cmd_1b(file, interval_param)) < 0 ) {
    return res;
  }
  if ((res = i2c_write_cmd_1b(file, end_page)) < 0 ) {
    return res;
  }
  if ((res = i2c_write_cmd_1b(file, 0x00)) < 0 ) {
    return res;
  }
  if ((res = i2c_write_cmd_1b(file, 0xff)) < 0 ) {
    return res;
  }

  return 0;
}

int ssd1306_setup_scroll(int file, bool left, uint8_t start_page, uint8_t end_page, int interval, uint8_t vertical_offset) {
  int res;

  /*
   * NOTE: this is horizontal + vertical scroll mode.
   * There is no vertical-only scroll mode.
   */

  if ((start_page > 0x07) || (end_page > 0x07) || (start_page > end_page)) {
    return -EINVAL;
  }

  if (vertical_offset > 0x3f) {
    return -EINVAL;
  }

  uint8_t interval_param;
  if ((res = ssd1306_interval_to_param(interval, &interval_param)) < 0) {
    return res;
  }

  if ((res = i2c_write_cmd_1b(file, left ? 0x2a : 0x29)) < 0 ) {
    return res;
  }
  if ((res = i2c_write_cmd_1b(file, 0x00)) < 0 ) {
    return res;
  }
  if ((res = i2c_write_cmd_1b(file, start_page)) < 0 ) {
    return res;
  }
  if ((res = i2c_write_cmd_1b(file, interval_param)) < 0 ) {
    return res;
  }
  if ((res = i2c_write_cmd_1b(file, end_page)) < 0 ) {
    return res;
  }
  if ((res = i2c_write_cmd_1b(file, vertical_offset)) < 0 ) {
    return res;
  }

  return 0;
}

int ssd1306_set_scroll(int file, bool enable) {
  /*
   * NOTE: after disabling the scrolling, "the ram data needs to be
   * rewritten."
   * The lastest scrolling setting will take effect once scrolling is enabled.
   */

  return i2c_write_cmd_1b(file, enable ? 0x2f : 0x2e);
}

int ssd1306_set_vertical_scroll_area(int file, uint8_t row_title, uint8_t roll_scroll) {
  int res;

  /*
   * NOTE: additional constraints apply, hardware may reject without notice.
   * row_title + roll_scroll < MUX_RATIO
   * vertical_offset < roll_scroll
   * display_start < roll_scroll
   *
   * row_title:   fixed roll of pixels on the top that will not scroll.
   * roll_scroll: roll of pixels that are scrolling.
   * (any leftovers will become fixed bottom roll of pixels.)
   */

  if ((row_title > 0x3f) || (roll_scroll > 0x7f)) {
    return -EINVAL;
  }

  if ((res = i2c_write_cmd_1b(file, 0xa3)) < 0 ) {
    return res;
  }
  if ((res = i2c_write_cmd_1b(file, row_title)) < 0 ) {
    return res;
  }
  if ((res = i2c_write_cmd_1b(file, roll_scroll)) < 0 ) {
    return res;
  }

  return 0;
}

int ssd1306_reset_vertical_scroll_area(int file) {
  return ssd1306_set_vertical_scroll_area(file, 0, 64);
}

int ssd1306_set_col_start(int file, uint8_t col) {
  int res;

  /*
   * NOTE: this is a 2-step process requiring splitting the parameter into high
   * and low half-byte and embedding the parameter in 2 commands and sending
   * them.
   * For page addressing mode only.
   */

  if ((res = i2c_write_cmd_1b(file, 0x00 | (col & 0x0f))) < 0 ) {
    return res;
  }
  if ((res = i2c_write_cmd_1b(file, 0x01 | (col > 4))) < 0 ) {
    return res;
  }

  return 0;
}

int ssd1306_reset_col_start(int file) {
  return ssd1306_set_col_start(file, 0);
}

#define SSD1306_MEMMODE_H    (0x00) /* Horizontally placed 1x8 blocks, not pixels! */
#define SSD1306_MEMMODE_V    (0x01)
#define SSD1306_MEMMODE_PAGE (0x02)

int ssd1306_set_mem_addr_mode(int file, uint8_t mode) {
  int res;

  if (mode > 0x02) {
    return -EINVAL;
  }

  if ((res = i2c_write_cmd_1b(file, 0x20)) < 0 ) {
    return res;
  }
  if ((res = i2c_write_cmd_1b(file, mode)) < 0 ) {
    return res;
  }

  return 0;
}

int ssd1306_reset_mem_addr_mode(int file) {
  return ssd1306_set_mem_addr_mode(file, SSD1306_MEMMODE_PAGE);
}

int ssd1306_set_col_addr(int file, uint8_t start, uint8_t end) {
  int res;

  /* NOTE: for horizontal or vertical addressing mode only. */
  if ((start > 0x7f) || (end > 0x7f) || (start > end)) {
    return -EINVAL;
  }

  if ((res = i2c_write_cmd_1b(file, 0x21)) < 0 ) {
    return res;
  }
  if ((res = i2c_write_cmd_1b(file, start)) < 0 ) {
    return res;
  }
  if ((res = i2c_write_cmd_1b(file, end)) < 0 ) {
    return res;
  }

  return 0;
}

int ssd1306_reset_col_addr(int file) {
  return ssd1306_set_col_addr(file, 0, 127);
}

int ssd1306_set_page_addr(int file, uint8_t start, uint8_t end) {
  int res;

  /*
   * NOTE: "for horizontal or vertical addressing mode", or should be for page
   * mode?
   */
  if ((start > 0x07) || (end > 0x07) || (start > end)) {
    return -EINVAL;
  }

  if ((res = i2c_write_cmd_1b(file, 0x22)) < 0 ) {
    return res;
  }
  if ((res = i2c_write_cmd_1b(file, start)) < 0 ) {
    return res;
  }
  if ((res = i2c_write_cmd_1b(file, end)) < 0 ) {
    return res;
  }

  return 0;
}

int ssd1306_reset_page_addr(int file) {
  return ssd1306_set_page_addr(file, 0, 7);
}

int ssd1306_set_page_start(int file, uint8_t page) {
  /* NOTE: "set GDDRAM page start address", for page addressing mode only. */

  if (page > 0x07) {
    return -EINVAL;
  }

  return i2c_write_cmd_1b(file, 0xb0 | (page & 0x07));
}

int ssd1306_set_start_line(int file, uint8_t line) {
  if (line > 0x3f) {
    return -EINVAL;
  }

  return i2c_write_cmd_1b(file, 0x40 | (line & 0x3f));
}

int ssd1306_reset_start_line(int file) {
  return ssd1306_set_start_line(file, 0);
}

int ssd1306_set_segment_remap(int file, bool reverse) {
  /* NOTE: normal = col 0 is seg 0, reverse = col 127 is seg 0. */

  return i2c_write_cmd_1b(file, reverse ? 0xa1 : 0xa0);
}

int ssd1306_reset_segment_remap(int file) {
  return ssd1306_set_segment_remap(file, false);
}

int ssd1306_set_mux_ratio(int file, int ratio) {
  int res;

  /* NOTE: controlled by how many line (COM) your display has. */

  if (ratio > 0x40) {
    return -EINVAL;
  }

  if ((res = i2c_write_cmd_1b(file, 0xa8)) < 0 ) {
    return res;
  }
  if ((res = i2c_write_cmd_1b(file, (ratio - 1) & 0x3f)) < 0 ) {
    return res;
  }

  return 0;
}

int ssd1306_reset_mux_ratio(int file) {
  return ssd1306_set_mux_ratio(file, 64);
}

int ssd1306_set_com_scan(int file, bool reverse) {
  /* NOTE: normal = line 0 is com 0, reverse = line (mux_ratio - 1) is com 0. */

  return i2c_write_cmd_1b(file, reverse ? 0xc8 : 0xc0);
}

int ssd1306_reset_com_scan(int file) {
  return ssd1306_set_com_scan(file, false);
}

int ssd1306_set_display_offset(int file, uint8_t offset) {
  int res;

  /* NOTE: start display on line <offset>. */

  if (offset > 0x3f) {
    return -EINVAL;
  }

  if ((res = i2c_write_cmd_1b(file, 0xd3)) < 0 ) {
    return res;
  }
  if ((res = i2c_write_cmd_1b(file, offset)) < 0 ) {
    return res;
  }

  return 0;
}

int ssd1306_reset_display_offset(int file) {
  return ssd1306_set_display_offset(file, 0);
}

int ssd1306_set_com_pin(int file, bool alternate, bool remap) {
  int res;

  /* NOTE: highly hardware-specific. */

  if ((res = i2c_write_cmd_1b(file, 0xda)) < 0 ) {
    return res;
  }
  if ((res = i2c_write_cmd_1b(file, 0x02 | (alternate ? 0x10 : 0x00) | (remap ? 0x20 : 0x00))) < 0 ) {
    return res;
  }

  return 0;
}

int ssd1306_reset_com_pin(int file) {
  return ssd1306_set_com_pin(file, true, false);
}

int ssd1306_set_clkdiv(int file, uint8_t ratio, uint8_t fosc) {
  int res;

  if ((ratio > 0x10) || (0 == ratio) || (fosc > 0x0f)) {
    return -EINVAL;
  }

  if ((res = i2c_write_cmd_1b(file, 0xd5)) < 0 ) {
    return res;
  }
  if ((res = i2c_write_cmd_1b(file, (fosc << 4) | (ratio - 1))) < 0 ) {
    return res;
  }

  return 0;
}

int ssd1306_reset_clkdiv(int file) {
  return ssd1306_set_clkdiv(file, 1, 8);
}

int ssd1306_set_precharge(int file, uint8_t phase1, uint8_t phase2) {
  int res;

  /* NOTE: phase1 and phase2 has unit of clock cycles. */
  if ((0 == phase1) || (0 == phase2) || (phase1 > 0x0f) || (phase1 > 0x0f)) {
    return -EINVAL;
  }

  if ((res = i2c_write_cmd_1b(file, 0xd9)) < 0 ) {
    return res;
  }
  if ((res = i2c_write_cmd_1b(file, (phase2 << 4) | phase1)) < 0 ) {
    return res;
  }

  return 0;
}

int ssd1306_reset_precharge(int file) {
  return ssd1306_set_precharge(file, 2, 2);
}

#define SSD1306_VCOMH_LEVEL_650MV 0
#define SSD1306_VCOMH_LEVEL_770MV 2
#define SSD1306_VCOMH_LEVEL_830MV 3

int ssd1306_set_vcomh_desel(int file, uint8_t level_code) {
  int res;

  /*
   * NOTE: although datasheet only gives voltages for 3 configurations, all
   * from 0~7 are possible.
   */

  if (level_code > 0x07) {
    return -EINVAL;
  }

  if ((res = i2c_write_cmd_1b(file, 0xdb)) < 0 ) {
    return res;
  }
  if ((res = i2c_write_cmd_1b(file, (level_code & 0x07) << 4)) < 0 ) {
    return res;
  }

  return 0;
}

int ssd1306_reset_vcomh_desel(int file) {
  return ssd1306_set_vcomh_desel(file, SSD1306_VCOMH_LEVEL_770MV);
}

int ssd1306_send_nop(int file) {
  return i2c_write_cmd_1b(file, 0xe3);
}

/*
 * NOTE: the following 3 are added in the new versions of the datasheet,
 * however, the charge pump enable is essential for most modules to operate.
 */
int ssd1306_set_fade(int file, bool fade_out, bool fade_in, uint8_t fade_interval) {
  int res;

  if (fade_interval > 128) {
    return -EINVAL;
  }
  if (fade_interval < 8) {
    fade_interval = 8;
  }

  if ((res = i2c_write_cmd_1b(file, 0x23)) < 0 ) {
    return res;
  }
  if ((res = i2c_write_cmd_1b(file, (fade_out ? 0x20 : 0x00) | (fade_in ? 0x10 : 0x00) | ((fade_interval / 8 - 1) & 0x0f))) < 0 ) {
    return res;
  }

  return 0;
}

int ssd1306_reset_fade(int file) {
  /* NOTE: default does not include fade_interval. */

  return ssd1306_set_fade(file, false, false, 8);
}

int ssd1306_set_zoom(int file, bool enable) {
  int res;

  /* NOTE: for panels in alternate COM configuration only. */

  if ((res = i2c_write_cmd_1b(file, 0xd6)) < 0 ) {
    return res;
  }
  if ((res = i2c_write_cmd_1b(file, enable ? 0x01 : 0x00)) < 0 ) {
    return res;
  }

  return 0;
}

int ssd1306_reset_zoom(int file) {
  return ssd1306_set_zoom(file, false);
}

int ssd1306_set_charge_pump(int file, bool enable) {
  int res;

  if ((res = i2c_write_cmd_1b(file, 0x8d)) < 0 ) {
    return res;
  }
  if ((res = i2c_write_cmd_1b(file, 0x10 | (enable ? 0x04 : 0x00))) < 0 ) {
    return res;
  }

  return 0;
}

int ssd1306_reset_charge_pump(int file) {
  return ssd1306_set_charge_pump(file, false);
}

#define SSD1306_STATUS_DISP_OFF (1 << 6)
/* NOTE: all other bits in the reg are reserved. */
int ssd1306_read_status(int file, uint8_t *reg) {
  return i2c_read_byte(file, reg);
}

/* NOTE: "No data read is provided in serial mode operation." */

int ssd1306_soft_reset(int file) {
  int res, i;

  /*
   * Longest command has 6 parameters.
   * Send 6 NOPs to finish any currently pending command.
   */

  for (i = 0; i < 6; i ++) {
    if ((res = ssd1306_send_nop(file)) < 0 ) {
      return res;
    }
  }

  /* Fundamentals. TODO: consider sequence. */
  if ((res = ssd1306_reset_power(file)) < 0 ) {
    return res;
  }
  if ((res = ssd1306_reset_charge_pump(file)) < 0 ) {
    return res;
  }
  if ((res = ssd1306_reset_contrast(file)) < 0 ) {
    return res;
  }
  if ((res = ssd1306_reset_display_test(file)) < 0 ) {
    return res;
  }
  if ((res = ssd1306_reset_inverse(file)) < 0 ) {
    return res;
  }

  /*
   * Scrolling.
   * NOTE: Scrolling parameters are not reset.
   *       Assuming scrolling is disabled after POR.
   */
  if ((res = ssd1306_reset_vertical_scroll_area(file)) < 0 ) {
    return res;
  }
  if ((res = ssd1306_set_scroll(file, false)) < 0 ) {
    return res;
  }

  /* Addressing */
  if ((res = ssd1306_reset_col_start(file)) < 0 ) {
    return res;
  }
  if ((res = ssd1306_reset_mem_addr_mode(file)) < 0 ) {
    return res;
  }
  if ((res = ssd1306_reset_col_addr(file)) < 0 ) {
    return res;
  }
  if ((res = ssd1306_reset_page_addr(file)) < 0 ) {
    return res;
  }

  /* Hardware */
  if ((res = ssd1306_reset_start_line(file)) < 0 ) {
    return res;
  }
  if ((res = ssd1306_reset_segment_remap(file)) < 0 ) {
    return res;
  }
  if ((res = ssd1306_reset_mux_ratio(file)) < 0 ) {
    return res;
  }
  if ((res = ssd1306_reset_com_scan(file)) < 0 ) {
    return res;
  }
  if ((res = ssd1306_reset_display_offset(file)) < 0 ) {
    return res;
  }
  if ((res = ssd1306_reset_com_pin(file)) < 0 ) {
    return res;
  }

  /* Clocking */
  if ((res = ssd1306_reset_clkdiv(file)) < 0 ) {
    return res;
  }
  if ((res = ssd1306_reset_precharge(file)) < 0 ) {
    return res;
  }
  if ((res = ssd1306_reset_vcomh_desel(file)) < 0 ) {
    return res;
  }

  /* VFX */
  if ((res = ssd1306_reset_fade(file)) < 0 ) {
    return res;
  }
  if ((res = ssd1306_reset_zoom(file)) < 0 ) {
    return res;
  }

  return 0;
}

int ssd1306_init(int file, int col, int line) {
  int res;

  /* NOTE: use defualts whenever we can. <col> is not used. */

  if ((line <= 0) || (col <= 0)) {
    return -EINVAL;
  }
  if ((line > 64) || (col > 128)) {
    return -EINVAL;
  }
  if (((line % 8) != 0) || ((col % 8) != 0)) {
    return -EINVAL;
  }

  if ((res = ssd1306_soft_reset(file)) < 0 ) {
    return res;
  }

  /* Should be already off, just ensuring. */
  if ((res = ssd1306_set_power(file, false)) < 0 ) {
    return res;
  }
  if ((res = ssd1306_set_mux_ratio(file, line)) < 0 ) {
    return res;
  }
  if ((res = ssd1306_set_mem_addr_mode(file, SSD1306_MEMMODE_H)) < 0 ) {
    return res;
  }
  if ((res = ssd1306_set_segment_remap(file, true)) < 0 ) {
    return res;
  }
  if ((res = ssd1306_set_com_scan(file, true)) < 0 ) {
    return res;
  }

  if ((res = ssd1306_set_charge_pump(file, true)) < 0 ) {
    return res;
  }
  if ((res = ssd1306_set_power(file, true)) < 0 ) {
    return res;
  }

  return 0;
}

int ssd1306_cls(int file, int col, int line) {
  int res;
  uint8_t *buf = NULL;
  const size_t len = line * col / 8 + 1;

  if ((line <= 0) || (col <= 0)) {
    return -EINVAL;
  }
  if ((line > 64) || (col > 128)) {
    return -EINVAL;
  }
  if (((line % 8) != 0) || ((col % 8) != 0)) {
    return -EINVAL;
  }

  buf = (uint8_t *)malloc(len);
  if (NULL == buf) {
    perror("malloc");
    return -ENOMEM;
  }

  bzero(buf, len);
  buf[0] = SSD1306_CONT_DATA_HDR;
  res = i2c_write_data(file, buf, len);

  free(buf);
  return res;
}

#define GET_BIT(x, n) ((x) >> (n) & 0x01)
/*
 * NOTE: will allocate the buffer for all data + SSD1306_CONT_DATA_HDR.
 * Remember to free.
 * For internal use.
 */
int read_png(char *path, int col, int line, uint8_t *buf[], size_t *len) {
  uint8_t header[8];
  int res;
  int width, height, x, y;
  png_byte depth;
  png_structp png_ptr;
  png_infop info_ptr;
  png_bytep *row_pointers;
  FILE *fp = NULL;

  if ((NULL == path) || (NULL == buf) || (NULL == len)) {
    return -EINVAL;
  }
  if ((line <= 0) || (col <= 0)) {
    return -EINVAL;
  }
  if ((line > 64) || (col > 128)) {
    return -EINVAL;
  }
  if (((line % 8) != 0) || ((col % 8) != 0)) {
    return -EINVAL;
  }

  if (NULL == (fp = fopen(path, "rb"))) {
    perror("fopen");
    return -EIO;
  }

  fread(header, 1, 8, fp);
  if (0 != png_sig_cmp(header, 0, 8)) {
    perror("png_sig_cmp");
    fclose(fp);
    return -ENOENT;
  }

  if (NULL == (png_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL))) {
    perror("png_create_read_struct");
    fclose(fp);
    return -ENOMEM;
  }

  if (NULL == (info_ptr = png_create_info_struct(png_ptr))) {
    perror("png_create_info_struct");
    png_destroy_read_struct(&png_ptr, NULL, NULL);
    fclose(fp);
    return -ENOMEM;
  }

  /*
   * NOTE: libpng is efficient but quite dirty.
   * If any error happens afterwards, program will return to this point with an
   * non-zero return value.
   */
  if (0 != (res = setjmp(png_jmpbuf(png_ptr)))) {
    perror("libpng, init");
    png_destroy_info_struct(png_ptr, &info_ptr);
    png_destroy_read_struct(&png_ptr, NULL, NULL);
    fclose(fp);
    return res;
  }

  png_init_io(png_ptr, fp);
  png_set_sig_bytes(png_ptr, 8);

  png_read_info(png_ptr, info_ptr);

  width  = png_get_image_width(png_ptr, info_ptr);
  height = png_get_image_height(png_ptr, info_ptr);
  depth  = png_get_bit_depth(png_ptr, info_ptr);
  /* Just enable interlace handling, we do not need to know which format. */
  png_set_interlace_handling(png_ptr);

  if (1 != depth) {
    fputs("ERROR: only black and white images are allowed!\n", stderr);
    png_destroy_info_struct(png_ptr, &info_ptr);
    png_destroy_read_struct(&png_ptr, NULL, NULL);
    fclose(fp);
    return -ENOENT;
  }
  /* NOTE: need capabilities to read sprite. */
  if ((width != col) || ((height % line) != 0)) {
    fprintf(stderr, "ERROR: image size %d x %d mismatches the screen!\n", width, height);
    png_destroy_info_struct(png_ptr, &info_ptr);
    png_destroy_read_struct(&png_ptr, NULL, NULL);
    fclose(fp);
    return -ENOENT;
  }

  png_read_update_info(png_ptr, info_ptr);

  if (0 != (res = setjmp(png_jmpbuf(png_ptr)))) {
    perror("libpng, reading");
    png_destroy_info_struct(png_ptr, &info_ptr);
    png_destroy_read_struct(&png_ptr, NULL, NULL);
    fclose(fp);
    return res;
  }

  /* Bytes should be packed (8 horizontal pixels per byte). */
  size_t bpr = png_get_rowbytes(png_ptr, info_ptr);
  fprintf(stdout, "Allocating %zu x %u bytes of memory for image...\n", bpr, height);
  row_pointers = (png_bytep*)malloc(sizeof(png_bytep) * height);
  if (NULL == row_pointers) {
    perror("malloc");
    png_destroy_info_struct(png_ptr, &info_ptr);
    png_destroy_read_struct(&png_ptr, NULL, NULL);
    fclose(fp);
    return -ENOMEM;
  }
  for (y = 0; y < height; y ++) {
    row_pointers[y] = (png_byte*)malloc(bpr);
    if (NULL == row_pointers[y]) {
      perror("malloc");
      png_destroy_info_struct(png_ptr, &info_ptr);
      png_destroy_read_struct(&png_ptr, NULL, NULL);
      fclose(fp);
      return -ENOMEM;
    }
  }

  png_read_image(png_ptr, row_pointers);
  /*
   * Add header, flatten and copy into buffer.
   * Need to re-arrange into 1x8 blocks. PNG packs pixels horizontally,
   * but our display (always) does so vertically (128 line in 64 COM).
   * For sprites, the sending function should handle following frames by
   * backtracking after every frame and adding the header.
   */
  *len = height * bpr + height / line; /* Every screen needs a header. */
  fprintf(stdout, "Allocating %zu bytes of memory for buffer...\n", *len);
  *buf = malloc(*len);
  if (NULL == *buf) {
    perror("malloc");
    png_destroy_info_struct(png_ptr, &info_ptr);
    png_destroy_read_struct(&png_ptr, NULL, NULL);
    fclose(fp);
    return -ENOMEM;
  }
  size_t ptr = 0;
  for (y = 0; y < height / 8; y ++) {
    if (y % (line / 8) == 0) {
      /* First line (page) of a frame, time to insert header. */
      (*buf)[ptr] = SSD1306_CONT_DATA_HDR;
      ptr ++;
    }
    for (x = 0; x < width; x ++ ) {
      (*buf)[ptr] = (GET_BIT(row_pointers[y * 8 + 0][x / 8], 7 - (x % 8)) << 0) | (GET_BIT(row_pointers[y * 8 + 1][x / 8], 7 - (x % 8)) << 1)
                  | (GET_BIT(row_pointers[y * 8 + 2][x / 8], 7 - (x % 8)) << 2) | (GET_BIT(row_pointers[y * 8 + 3][x / 8], 7 - (x % 8)) << 3)
                  | (GET_BIT(row_pointers[y * 8 + 4][x / 8], 7 - (x % 8)) << 4) | (GET_BIT(row_pointers[y * 8 + 5][x / 8], 7 - (x % 8)) << 5)
                  | (GET_BIT(row_pointers[y * 8 + 6][x / 8], 7 - (x % 8)) << 6) | (GET_BIT(row_pointers[y * 8 + 7][x / 8], 7 - (x % 8)) << 7);
      ptr ++;
    }
  }
  /* TODO: check underrun / overrun if necessary. */

  png_destroy_info_struct(png_ptr, &info_ptr);
  png_destroy_read_struct(&png_ptr, NULL, NULL);
  for (y = 0; y < height; y++) {
    free(row_pointers[y]);
  }
  free(row_pointers);
  fclose(fp);

  return 0;
}

int ssd1306_send_png(int file, int col, int line, char *path) {
  int res;
  size_t len;
  uint8_t *buf = NULL;

  if (NULL == path) {
    return -EINVAL;
  }

  if ((res = read_png(path, col, line, &buf, &len)) != 0) {
    return res;
  }
  if (len != line * col / 8 + 1) {
    fprintf(stdout, "NOTE: expect %zu bytes of PNG data, got %zu bytes, image is probably sprites.\n", (size_t)(line * col / 8 + 1), len);
  }

  res = i2c_write_data(file, buf, len);

  if (buf != NULL) {
    free(buf);
  }
  return res;
}

/* Each pass of the sprite, for internal use. */
int ssd1306_send_png_sprite_pass(int file, uint8_t buf[], size_t len, size_t flen) {
  int res;

  if (((len % flen) != 0) || (NULL == buf)) {
    return -EINVAL;
  }

  while ((len > 0) && (!stop)) {
    if ((res = i2c_write_data(file, buf, flen)) < 0) {
      return res;
    }
    buf += flen;
    len -= flen;
  }

  return 0;
}

/*
 * NOTE:
 * loop == 0: loop forever until killed by signal.
 * loop == 1: no loop, single pass.
 */
int ssd1306_send_png_sprite(int file, int col, int line, char *path, int delay_ms, int loop) {
  int res;
  size_t len;
  struct sigaction sia;
  uint8_t *buf = NULL;

  if ((delay_ms < 0) || (delay_ms >= INT_MAX / 1000) || (loop < 0) || (NULL == path)) {
    return -EINVAL;
  }

  if ((res = read_png(path, col, line, &buf, &len)) != 0) {
    return res;
  }

  /* Setup SIGINT handler. NOTE: there is no need to unregister it manually. */
  bzero(&sia, sizeof(sia));
  sia.sa_handler = sigint_handler;
  stop = false;
  if ((res = sigaction(SIGINT, &sia, NULL)) < 0) {
    perror("sigaction");
    if (buf != NULL) {
      free(buf);
    }
    return res;
  }

  size_t flen = col * line / 8 + 1; /* Size of each frame. */
  if (loop == 0) {

    while (!stop) {
      if ((res = ssd1306_send_png_sprite_pass(file, buf, len, flen)) < 0) {
        if (buf != NULL) {
          free(buf);
        }
        return res;
      }
      usleep(delay_ms * 1000);
    }
  } else {
    for (; loop > 0; loop --) {
      if ((res = ssd1306_send_png_sprite_pass(file, buf, len, flen)) < 0) {
        if (buf != NULL) {
          free(buf);
        }
        return res;
      }
      usleep(delay_ms * 1000);
    }
  }

  bzero(&sia, sizeof(sia));
  sia.sa_handler = SIG_DFL;
  if ((res = sigaction(SIGINT, &sia, NULL)) < 0) {
    perror("sigaction unregister");
    if (buf != NULL) {
      free(buf);
    }
    return res;
  }
  if (buf != NULL) {
    free(buf);
  }
  return 0;
}

/*
 * TODO:
(send frame)
(load png)
(CLI)
(font & text?)
*/

int main (int argc, char *argv[]) {
  int file, res;

  if ((file = i2c_open(1)) < 0) {
    return file;
  }

  if ((res = i2c_select(file, SSD1306_DEVAD_A)) < 0) {
    return res;
  }

  // TEST ONLY
  ssd1306_init(file, 128, 64);
  ssd1306_cls(file, 128, 64); // SSD1306 may have a SRAM-based GDDRAM, some parts of the graphic are perserved after power cycle.
  ssd1306_send_png(file, 128, 64, "ui2c_ssd1306_test_static.png");
  sleep(1);
  ssd1306_send_png_sprite(file, 128, 64, "ui2c_ssd1306_test_sprite.png", 0, 0);
//  
//  sleep(1);
//  
//  sleep(1);
  // Do not have to send new frames, it will animate itself.

  return 0;
}
