#ifndef RING_BUFFER_H
#define RING_BUFFER_H

#include <Arduino.h>

template <size_t SIZE>
class RingBuffer {
private:
    char buffer[SIZE];
    volatile size_t head = 0;
    volatile size_t tail = 0;

public:
    bool push(char c) {
        size_t next = (head + 1) % SIZE;
        if (next == tail) return false; // Buffer Full
        buffer[head] = c;
        head = next;
        return true;
    }

    bool pop(char &c) {
        if (head == tail) return false; // Buffer Empty
        c = buffer[tail];
        tail = (tail + 1) % SIZE;
        return true;
    }

    bool isEmpty() const { return head == tail; }
};

#endif