/*
 * s40button — front-panel button daemon for the Lidl Silvercrest Gateway.
 *
 * Polls GPIO 9 every 100 ms; on a confirmed 5 s long-press, invokes
 * /usr/sbin/recover_efr32 -q to reset the EFR32 radio without rebooting
 * the SoC.  Replaces the v3.2.x/v3.3.0 busybox shell loop, which had an
 * intermittent SIGSEGV after some hours of idle polling (via
 * `devmem` + ash).
 *
 * Behaviour parity with the previous shell version:
 *   - Switch GPIO 9 from peripheral mode to GPIO input at startup
 *     (clear bit 9 of CNR @ 0x18003500, ensure bit 9 of DIR is 0).
 *   - 100 ms poll loop on DATA register bit 9 (active LOW).
 *   - Edge detection: press detector stays disarmed at boot until a HIGH
 *     is observed.  Guards against a stuck-LOW pin / wrong mux at boot.
 *   - Debounce: require 3 consecutive LOW samples (300 ms) before
 *     treating it as a real press.
 *   - Mux re-verification: re-read CNR before counting; if GPIO 9 was
 *     flipped back to peripheral mode at runtime, restore + disarm.
 *   - Subtle LED blink (every 500 ms, brightness alternates 30/255)
 *     during the hold for visual feedback.
 *   - 5 s sustained press fires recover_efr32; the LED briefly blinks
 *     off→on to confirm the trigger; we wait for release before re-arming.
 *   - Short presses (< 5 s) are ignored.
 *   - Every state transition is logged via syslog (LOG_USER).
 *
 * Build: build_s40button.sh in this tree (Lexra MIPS / musl, static).
 *
 * J. Nilo, April 2026
 */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/*
 * GPIO bank lives at physical 0x18003500.  mmap() requires a page-aligned
 * offset, so we map the enclosing 4 KB page (0x18003000) and bias the
 * register pointer by +0x500 after the mapping.
 */
#define PAGE_SIZE           0x1000u
#define GPIO_PHYS_BASE      0x18003500u
#define GPIO_PAGE_BASE      (GPIO_PHYS_BASE & ~(PAGE_SIZE - 1u))
#define GPIO_PAGE_OFFSET    (GPIO_PHYS_BASE & (PAGE_SIZE - 1u))

#define CNR_OFFSET          0x00u
#define DIR_OFFSET          0x08u
#define DATA_OFFSET         0x0Cu

#define BUTTON_BIT          9
#define BUTTON_MASK         (1u << BUTTON_BIT)

#define POLL_INTERVAL_MS    100
#define LONG_PRESS_MS       5000
#define DEBOUNCE_SAMPLES    3

#define LED_PATH            "/sys/class/leds/status/brightness"
#define RECOVER_BIN         "/usr/sbin/recover_efr32"

static volatile uint32_t *g_regs;

static inline uint32_t reg_read(unsigned offset)
{
    return g_regs[offset / 4];
}

static inline void reg_write(unsigned offset, uint32_t val)
{
    g_regs[offset / 4] = val;
}

static int gpio_in_peripheral_mode(void)
{
    return (reg_read(CNR_OFFSET) & BUTTON_MASK) != 0;
}

static int button_pressed(void)
{
    /* Active LOW: pressed = bit 9 reads 0. */
    return (reg_read(DATA_OFFSET) & BUTTON_MASK) == 0;
}

static void configure_gpio(void)
{
    /* Clear bit 9 of CNR → switch GPIO 9 to GPIO mode. */
    reg_write(CNR_OFFSET, reg_read(CNR_OFFSET) & ~BUTTON_MASK);
    /* Clear bit 9 of DIR → input. */
    reg_write(DIR_OFFSET, reg_read(DIR_OFFSET) & ~BUTTON_MASK);
}

static int led_get(void)
{
    int v = -1;
    FILE *f = fopen(LED_PATH, "r");
    if (!f)
        return -1;
    if (fscanf(f, "%d", &v) != 1)
        v = -1;
    fclose(f);
    return v;
}

static void led_set(int value)
{
    FILE *f = fopen(LED_PATH, "w");
    if (!f)
        return;
    fprintf(f, "%d\n", value);
    fclose(f);
}

static void msleep(unsigned ms)
{
    struct timespec ts = {
        .tv_sec = ms / 1000,
        .tv_nsec = (long)(ms % 1000) * 1000000L,
    };
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR)
        ; /* resume on signal */
}

static void run_recover_efr32(void)
{
    pid_t pid = fork();
    if (pid == 0) {
        execl(RECOVER_BIN, "recover_efr32", "-q", (char *)NULL);
        _exit(127);
    }
    if (pid > 0) {
        int status;
        waitpid(pid, &status, 0);
        if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
            syslog(LOG_WARNING, "recover_efr32 exited with status %d",
                   WEXITSTATUS(status));
        }
    } else {
        syslog(LOG_ERR, "fork() failed: %s", strerror(errno));
    }
}

int main(void)
{
    int fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0) {
        fprintf(stderr, "s40button: open /dev/mem: %s\n", strerror(errno));
        return 1;
    }
    void *page = mmap(NULL, PAGE_SIZE, PROT_READ | PROT_WRITE,
                      MAP_SHARED, fd, GPIO_PAGE_BASE);
    close(fd);
    if (page == MAP_FAILED) {
        fprintf(stderr, "s40button: mmap GPIO: %s\n", strerror(errno));
        return 1;
    }
    g_regs = (volatile uint32_t *)((char *)page + GPIO_PAGE_OFFSET);

    openlog("s40button", LOG_PID, LOG_USER);
    configure_gpio();
    syslog(LOG_NOTICE,
           "started: GPIO 9 configured, %dms poll, %ds long-press → %s -q",
           POLL_INTERVAL_MS, LONG_PRESS_MS / 1000, RECOVER_BIN);

    int saved_led = led_get();
    int armed = 0;

    for (;;) {
        if (!button_pressed()) {
            if (!armed) {
                syslog(LOG_NOTICE,
                       "GPIO 9 idle (HIGH), press detector armed");
                armed = 1;
            }
            msleep(POLL_INTERVAL_MS);
            continue;
        }

        /* Line is LOW. */
        if (!armed) {
            msleep(POLL_INTERVAL_MS);
            continue;
        }

        if (gpio_in_peripheral_mode()) {
            syslog(LOG_WARNING,
                   "GPIO 9 reverted to peripheral mode (CNR=0x%08x), "
                   "restoring + disarming",
                   reg_read(CNR_OFFSET));
            configure_gpio();
            armed = 0;
            msleep(200);
            continue;
        }

        /* Debounce. */
        int debounce = 1;
        while (debounce < DEBOUNCE_SAMPLES) {
            msleep(POLL_INTERVAL_MS);
            if (button_pressed()) {
                debounce++;
            } else {
                debounce = 0;
                break;
            }
        }
        if (debounce < DEBOUNCE_SAMPLES)
            continue;

        /* Confirmed press. */
        syslog(LOG_NOTICE,
               "press detected, watching for %ds long-press",
               LONG_PRESS_MS / 1000);
        int held_ms = DEBOUNCE_SAMPLES * POLL_INTERVAL_MS;
        int blink_state = 0;
        int fired = 0;

        while (button_pressed()) {
            if (held_ms % 500 == 0) {
                led_set(blink_state ? 255 : 30);
                blink_state = !blink_state;
            }
            if (held_ms >= LONG_PRESS_MS) {
                led_set(0);
                msleep(200);
                led_set(255);
                syslog(LOG_NOTICE,
                       "long-press detected, invoking recover_efr32");
                run_recover_efr32();
                fired = 1;
                /* Wait for release before re-arming. */
                while (button_pressed())
                    msleep(200);
                armed = 0;
                break;
            }
            msleep(POLL_INTERVAL_MS);
            held_ms += POLL_INTERVAL_MS;
        }

        if (!fired) {
            syslog(LOG_NOTICE,
                   "press released after %dms (short-press, ignored)",
                   held_ms);
        }

        if (saved_led >= 0)
            led_set(saved_led);
    }
    /* unreachable */
}
