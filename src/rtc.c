#include <cpu.h>
#include <io.h>

/*
 * the cmos clock, the little battery-backed chip that keeps the time while
 * the computer is off. you pick a register by writing its number to port 0x70
 * and read it back from 0x71. depending on how the chip is set up (register
 * b), the numbers come either as plain binary or as bcd, and the hour either
 * in 12 or 24 hour form, so both get tidied up here. we also wait for the
 * "update in progress" flag to clear first, so we never catch the clock in
 * the middle of ticking over.
 */

/*
 * picking the register and reading it have to happen back to back, so
 * interrupts stay off in between and no other task can sneak in.
 */
static uint8_t cmos_read(uint8_t reg)
{
    uint32_t flags = irq_save();
    outb(0x70, reg);
    uint8_t v = inb(0x71);
    irq_restore(flags);
    return v;
}

static uint8_t from_bcd(uint8_t v)
{
    return (v & 0x0F) + (v >> 4) * 10;
}

/*
 * reads the hour, minute and second, turning bcd and 12 hour times into plain
 * 24 hour numbers.
 */
void rtc_time(int *hour, int *minute, int *second)
{
    while (cmos_read(0x0A) & 0x80)
        ;

    uint8_t s = cmos_read(0x00);
    uint8_t m = cmos_read(0x02);
    uint8_t h = cmos_read(0x04);
    uint8_t format = cmos_read(0x0B);

    bool pm = h & 0x80;
    h &= 0x7F;
    if (!(format & 0x04)) {
        s = from_bcd(s);
        m = from_bcd(m);
        h = from_bcd(h);
    }
    /*
     * AI Contribution (Bug Fix): on a 12 hour clock, 12 am is midnight and
     * 12 pm is noon, so the 12 comes off before pm adds 12 back. adding 12
     * for pm alone turned noon into midnight, and left 12 am as noon.
     */
    if (!(format & 0x02))
        h = h % 12 + (pm ? 12 : 0);

    *hour = h;
    *minute = m;
    *second = s;
}

/*
 * the date. the chip only keeps two digits of the year, so we assume it's the
 * 2000s.
 */
void rtc_date(int *year, int *month, int *day)
{
    while (cmos_read(0x0A) & 0x80)
        ;

    uint8_t d = cmos_read(0x07);
    uint8_t mo = cmos_read(0x08);
    uint8_t y = cmos_read(0x09);
    if (!(cmos_read(0x0B) & 0x04)) {
        d = from_bcd(d);
        mo = from_bcd(mo);
        y = from_bcd(y);
    }

    *year = 2000 + y;
    *month = mo;
    *day = d;
}
