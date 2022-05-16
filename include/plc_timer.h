#include <stopwatch.h>

// On-delay timer, Q is ON after DIN is TRUE for PT duration
class TON
{
    uint32_t pt, lastMillis;
    bool input;
    Stopwatch sw;

public:
    TON(uint32_t _pt) : pt(_pt) {}
    void setPT(uint32_t _pt) { pt = _pt; }
    uint32_t getPT() { return pt; }
    // Timer will count as long as din is hold true, and will be reset when reset input is true
    void IN(bool din, bool reset = false)
    {
        if (din && !input)
            sw.start();

        if (!din && input)
            sw.reset();
        input = din;
        if (reset)
            sw.reset();
    }
    // Q will on if din true for preset time
    bool Q() { return sw.elapsed() > pt; }
    // ET will increase when timer is counting and pt when it's not
    uint32_t ET() { return (Q()) ? pt : sw.elapsed(); }
};
// Off-delay timer, Q is ON after rising edge of DIN, and OFF when ET>PT after falling edge of DIN
class TOFF
{
    Stopwatch sw;
    bool input = false;
    bool q = false;
    uint32_t et = 0;
    uint32_t pt = 0;
    void CheckElapsedTime()
    {
        et = sw.elapsed();
        if (et > pt)
        {
            q = false;
            et = pt;
        }
    }

public:
    TOFF(uint32_t _pt) : pt(_pt) {}
    void setPT(uint32_t _pt) { pt = _pt; }
    uint32_t getPT() { return pt; }
    // Timer will count after falling edge of din, if din became true again then timer count reset, and timer will reset when reset input is true
    void IN(bool din, bool reset = false)
    {
        if (!input && din)
        {
            sw.reset();
            q = true;
        }
        if (input && !din)
            sw.start();
        input = din;
        if (reset)
            sw.reset();
    }
    // Timer output. Output Q becomes TRUE when IN is TRUE and while timer is counting, and ELSE if et>pt after falling edge of din.
    bool Q()
    {
        CheckElapsedTime();
        return q;
    }
    // ET will increase when timer is counting and pt when it's not
    int ET()
    {
        CheckElapsedTime();
        return et;
    }
};
// Pulsed-timer, Q is ON after rising edge of DIN and OFF when ET>PT
class TP
{
    Stopwatch sw;

    bool input = false;
    uint32_t et = 0, pt = 0;

    void CheckElapsedTime()
    {
        if (sw.isRunning())
            et = sw.elapsed();

        if (et > pt)
        {
            sw.reset();
            et = pt;
        }
    }

public:
    TP(uint32_t _pt) : pt(_pt) {}
    uint32_t getPT() { return pt; }
    void setPT(uint32_t _pt) { pt = _pt; }

    // Rising edge on input will count the timer, and reset timer when reset input is true
    void IN(bool din, bool reset = false)
    {
        if (reset)
            sw.reset();
        if (sw.isRunning())
            return;
        if (!input && din && !sw.isRunning())
            sw.start();

        if (input && !din && !sw.isRunning())
            et = 0;
        input = din;

        if (reset)
            sw.reset();
    }

    // true while timer is counting and false while it's not
    bool Q()
    {
        CheckElapsedTime();
        return sw.isRunning();
    }
    // Increases when timer is counting (after rising edge on input)
    uint32_t ET()
    {
        CheckElapsedTime();
        return et;
    }
};