#include "pipe.h"
#include "log.h"

#include <windows.h>

#include <atomic>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

namespace
{
    constexpr size_t kMaxQueued = 1024;     // per direction; the oldest incoming message goes first when full
    constexpr DWORD kRetryMs = 2000;
    constexpr DWORD kWriteTimeoutMs = 5000;

    struct Connection
    {
        std::string name;
        std::wstring path;
        std::mutex lock;
        std::deque<Pipes::Event> in;
        std::deque<std::string> out;
        HANDLE wake = nullptr;          // set when there is something to send, or to stop
        std::atomic<bool> stop{false};
        std::atomic<bool> connected{false};
        bool dropped = false;           // logged once per connection that the incoming queue overflowed
    };

    std::mutex g_lock;
    std::map<int, std::shared_ptr<Connection>> g_connections;
    int g_next = 1;

    std::wstring widen(const std::string& s)
    {
        std::wstring w(MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), static_cast<int>(w.size()));
        return w;
    }

    void push(Connection& c, Pipes::Event e)
    {
        std::lock_guard guard(c.lock);
        if (c.in.size() >= kMaxQueued)
        {
            c.in.pop_front();
            if (!c.dropped)
            {
                c.dropped = true;
                LOG("Pipe {}: the mod does not keep up, dropping the oldest messages", c.name);
            }
        }
        c.in.push_back(std::move(e));
    }

    // Writes all of `data`; false on an error or timeout.
    bool writeAll(HANDLE pipe, HANDLE event, const std::string& data)
    {
        OVERLAPPED ov{};
        ov.hEvent = event;
        DWORD written = 0;
        if (!WriteFile(pipe, data.data(), static_cast<DWORD>(data.size()), &written, &ov))
        {
            if (GetLastError() != ERROR_IO_PENDING)
            {
                return false;
            }
            if (WaitForSingleObject(event, kWriteTimeoutMs) != WAIT_OBJECT_0)
            {
                CancelIo(pipe);
                return false;
            }
            if (!GetOverlappedResult(pipe, &ov, &written, FALSE))
            {
                return false;
            }
        }
        return written == data.size();
    }

    // One connected session: reads frames and sends the queue until an error or stop.
    void session(Connection& c, HANDLE pipe)
    {
        HANDLE readEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        HANDLE writeEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        std::string buffer;
        char chunk[4096];
        bool ok = readEvent && writeEvent;
        while (ok && !c.stop)
        {
            OVERLAPPED ov{};
            ov.hEvent = readEvent;
            ResetEvent(readEvent);
            DWORD got = 0;
            bool pending = false;
            if (!ReadFile(pipe, chunk, sizeof(chunk), &got, &ov))
            {
                const DWORD error = GetLastError();
                if (error == ERROR_IO_PENDING)
                {
                    pending = true;
                }
                else if (error != ERROR_MORE_DATA)
                {
                    break;
                }
            }
            while (pending && ok && !c.stop)
            {
                const HANDLE waits[] = {readEvent, c.wake};
                const DWORD w = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
                if (w == WAIT_OBJECT_0)
                {
                    if (!GetOverlappedResult(pipe, &ov, &got, FALSE) && GetLastError() != ERROR_MORE_DATA)
                    {
                        ok = false;
                    }
                    pending = false;
                }
                else if (w == WAIT_OBJECT_0 + 1)
                {
                    std::deque<std::string> out;
                    {
                        std::lock_guard guard(c.lock);
                        out.swap(c.out);
                    }
                    for (const std::string& frame : out)
                    {
                        if (!writeAll(pipe, writeEvent, frame))
                        {
                            ok = false;
                            break;
                        }
                    }
                }
                else
                {
                    ok = false;
                }
            }
            if (pending)
            {
                CancelIo(pipe);
                GetOverlappedResult(pipe, &ov, &got, TRUE);
                break;
            }
            buffer.append(chunk, got);
            while (buffer.size() >= 2)
            {
                const size_t length = static_cast<uint8_t>(buffer[0]) | (static_cast<uint8_t>(buffer[1]) << 8);
                if (buffer.size() < 2 + length)
                {
                    break;
                }
                push(c, {Pipes::Event::Message, buffer.substr(2, length)});
                buffer.erase(0, 2 + length);
            }
        }
        if (readEvent)
        {
            CloseHandle(readEvent);
        }
        if (writeEvent)
        {
            CloseHandle(writeEvent);
        }
    }

    void run(std::shared_ptr<Connection> c)
    {
        bool announced = false;
        while (!c->stop)
        {
            HANDLE pipe = CreateFileW(c->path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                      FILE_FLAG_OVERLAPPED, nullptr);
            if (pipe == INVALID_HANDLE_VALUE)
            {
                if (!announced)
                {
                    announced = true;
                    LOG("Pipe {}: no server yet, retrying every {} s", c->name, kRetryMs / 1000);
                }
                WaitForSingleObject(c->wake, kRetryMs);
                continue;
            }
            announced = false;
            LOG("Pipe {}: connected", c->name);
            c->connected = true;
            push(*c, {Pipes::Event::Connected, {}});
            SetEvent(c->wake);      // send what was queued before the connection
            session(*c, pipe);
            CloseHandle(pipe);
            c->connected = false;
            push(*c, {Pipes::Event::Disconnected, {}});
            LOG("Pipe {}: disconnected", c->name);
            if (!c->stop)
            {
                WaitForSingleObject(c->wake, kRetryMs);
            }
        }
        CloseHandle(c->wake);
    }

    std::shared_ptr<Connection> find(int handle)
    {
        std::lock_guard guard(g_lock);
        const auto it = g_connections.find(handle);
        return it == g_connections.end() ? nullptr : it->second;
    }
}

int Pipes::open(const std::string& name)
{
    if (name.empty() || name.size() > 200 || name.find_first_of("\\/:") != std::string::npos)
    {
        return 0;
    }
    auto c = std::make_shared<Connection>();
    c->name = name;
    c->path = L"\\\\.\\pipe\\" + widen(name);
    c->wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!c->wake)
    {
        return 0;
    }
    int handle = 0;
    {
        std::lock_guard guard(g_lock);
        handle = g_next++;
        g_connections[handle] = c;
    }
    // Detached: the thread owns its connection (shared) and ends when it is stopped; at process exit Windows ends it.
    std::thread(run, c).detach();
    return handle;
}

void Pipes::close(int handle)
{
    std::shared_ptr<Connection> c;
    {
        std::lock_guard guard(g_lock);
        const auto it = g_connections.find(handle);
        if (it == g_connections.end())
        {
            return;
        }
        c = it->second;
        g_connections.erase(it);
    }
    c->stop = true;
    SetEvent(c->wake);
}

bool Pipes::send(int handle, const std::string& text)
{
    const auto c = find(handle);
    if (!c || text.size() > 0xFFFF)
    {
        return false;
    }
    std::string frame;
    frame.reserve(text.size() + 2);
    frame.push_back(static_cast<char>(text.size() & 0xFF));
    frame.push_back(static_cast<char>(text.size() >> 8));
    frame += text;
    {
        std::lock_guard guard(c->lock);
        if (c->out.size() >= kMaxQueued)
        {
            return false;
        }
        c->out.push_back(std::move(frame));
    }
    SetEvent(c->wake);
    return true;
}

std::vector<Pipes::Event> Pipes::poll(int handle, size_t limit)
{
    std::vector<Event> events;
    const auto c = find(handle);
    if (!c)
    {
        return events;
    }
    std::lock_guard guard(c->lock);
    while (!c->in.empty() && events.size() < limit)
    {
        events.push_back(std::move(c->in.front()));
        c->in.pop_front();
    }
    return events;
}

bool Pipes::connected(int handle)
{
    const auto c = find(handle);
    return c && c->connected;
}
