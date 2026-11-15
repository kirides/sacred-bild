#pragma once
#include <cstddef>
#include <string>
#include <vector>

// Named pipe clients for mods that talk to a program outside the game (a Twitch integration, a stream overlay).
// A mod may only open the pipes its mod.lua lists (`pipes = {...}`); the kernel checks that. Each pipe has a thread
// of its own that connects (and reconnects every two seconds while the server is not there), reads and writes;
// the engine thread only polls the queue, so a slow or absent server never stalls a frame.
//
// Messages are framed as on the risen-twitch pipe: a 16-bit little-endian length, then that many bytes (UTF-8 text,
// usually JSON).
namespace Pipes
{
    struct Event
    {
        enum Kind { Connected, Disconnected, Message } kind;
        std::string text;   // the message (Message only)
    };

    // `name` without the \\.\pipe\ prefix. A handle > 0, or 0 if the name is not allowed (empty, a path).
    int open(const std::string& name);

    // Stops the connection; the handle is invalid afterwards.
    void close(int handle);

    // Queues a message (at most 65535 bytes); false if the handle is unknown, the message is too long, or the queue
    // is full (the server does not read).
    bool send(int handle, const std::string& text);

    // The events since the last poll, oldest first, at most `limit` (the rest stay queued).
    std::vector<Event> poll(int handle, size_t limit);

    // Whether the pipe is connected now.
    bool connected(int handle);
}
