#include <Arduino.h>

class Stopwatch
{
    typedef enum
    {
        RESET,
        RUNNING,
        STOPPED
    } State;
    uint32_t startTime, stopTime;
    State state = RESET;

public:
    Stopwatch() { reset(); }
    bool isRunning() { return state == RUNNING; }
    void start()
    {
        if (state == RESET || state == STOPPED)
        {
            state = RUNNING;
            uint32_t t = millis();
            startTime += t - stopTime;
            stopTime = t;
        }
    }
    void stop()
    {
        if (state == RUNNING)
        {
            state = STOPPED;
            stopTime = millis();
        }
    }
    void reset()
    {
        state = RESET;
        startTime = stopTime = 0;
    }
    uint32_t elapsed() const
    {
        if (state == RUNNING)
            return millis() - startTime;
        return stopTime - startTime;
    }
};