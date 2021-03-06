/*
  ==============================================================================

   This file is part of the JUCE library.
   Copyright (c) 2020 - Raw Material Software Limited

   JUCE is an open source library subject to commercial or open-source
   licensing.

   The code included in this file is provided under the terms of the ISC license
   http://www.isc.org/downloads/software-support-policy/isc-license. Permission
   To use, copy, modify, and/or distribute this software for any purpose with or
   without fee is hereby granted provided that the above copyright notice and
   this permission notice appear in all copies.

   JUCE IS PROVIDED "AS IS" WITHOUT ANY WARRANTY, AND ALL WARRANTIES, WHETHER
   EXPRESSED OR IMPLIED, INCLUDING MERCHANTABILITY AND FITNESS FOR PURPOSE, ARE
   DISCLAIMED.

  ==============================================================================
*/

namespace juce
{

//==============================================================================
class InternalMessageQueue
{
public:
    InternalMessageQueue()
    {
        auto err = ::socketpair (AF_LOCAL, SOCK_STREAM, 0, msgpipe);
        jassert (err == 0);
        ignoreUnused (err);

        LinuxEventLoop::registerFdCallback (getReadHandle(),
                                            [this] (int fd)
                                            {
                                                while (auto msg = popNextMessage (fd))
                                                {
                                                    JUCE_TRY
                                                    {
                                                        msg->messageCallback();
                                                    }
                                                    JUCE_CATCH_EXCEPTION
                                                }
                                            });
    }

    ~InternalMessageQueue()
    {
        LinuxEventLoop::unregisterFdCallback (getReadHandle());

        close (getReadHandle());
        close (getWriteHandle());

        clearSingletonInstance();
    }

    //==============================================================================
    void postMessage (MessageManager::MessageBase* const msg) noexcept
    {
        ScopedLock sl (lock);
        queue.add (msg);

        if (bytesInSocket < maxBytesInSocketQueue)
        {
            bytesInSocket++;

            ScopedUnlock ul (lock);
            unsigned char x = 0xff;
            auto numBytes = write (getWriteHandle(), &x, 1);
            ignoreUnused (numBytes);
        }
    }

    //==============================================================================
    JUCE_DECLARE_SINGLETON (InternalMessageQueue, false)

private:
    CriticalSection lock;
    ReferenceCountedArray <MessageManager::MessageBase> queue;

    int msgpipe[2];
    int bytesInSocket = 0;
    static constexpr int maxBytesInSocketQueue = 128;

    int getWriteHandle() const noexcept  { return msgpipe[0]; }
    int getReadHandle() const noexcept   { return msgpipe[1]; }

    MessageManager::MessageBase::Ptr popNextMessage (int fd) noexcept
    {
        const ScopedLock sl (lock);

        if (bytesInSocket > 0)
        {
            --bytesInSocket;

            ScopedUnlock ul (lock);
            unsigned char x;
            auto numBytes = read (fd, &x, 1);
            ignoreUnused (numBytes);
        }

        return queue.removeAndReturn (0);
    }
};

JUCE_IMPLEMENT_SINGLETON (InternalMessageQueue)

//==============================================================================
struct InternalRunLoop
{
public:
    InternalRunLoop()
    {
        fdReadCallbacks.reserve (16);
    }

    void registerFdCallback (int fd, std::function<void (int)>&& cb, short eventMask)
    {
        const ScopedLock sl (pendingChangesLock);
        pollfd pfd = { fd, eventMask, 0 };
        pendingAdditions.push_back({ fd, std::move (cb), pfd });
    }

    void unregisterFdCallback (int fd)
    {
        const ScopedLock sl (pendingChangesLock);
        pendingRemovals.push_back(fd);
    }

private:
    // Immediately removes callbacks for `fd`. Not thread-safe.
    void removeFdCallback (int fd)
    {
        {
            auto removePredicate = [=] (const std::pair<int, std::function<void (int)>>& cb)  { return cb.first == fd; };

            fdReadCallbacks.erase (std::remove_if (std::begin (fdReadCallbacks), std::end (fdReadCallbacks), removePredicate),
                                   std::end (fdReadCallbacks));
        }

        {
            auto removePredicate = [=] (const pollfd& pfd)  { return pfd.fd == fd; };

            pfds.erase (std::remove_if (std::begin (pfds), std::end (pfds), removePredicate),
                        std::end (pfds));
        }
    }

    // Returns true if any changes were made.
    bool applyPendingChanges()
    {
        ScopedLock sl (pendingChangesLock);
        if (pendingAdditions.empty() && pendingRemovals.empty())
        {
            return false;
        }

        for (auto& addition : pendingAdditions)
        {
            int fd = std::get<0> (addition);
            auto& cb = std::get<1> (addition);
            pollfd pfd = std::get<2> (addition);

            fdReadCallbacks.push_back ({ fd, std::move (cb) });
            pfds.push_back (pfd);
        }

        for (int fd : pendingRemovals)
        {
            removeFdCallback (fd);
        }

        pendingAdditions.clear();
        pendingRemovals.clear();
        return true;
    }

public:
    bool dispatchPendingEvents()
    {
        const ScopedLock sl (lock);

        applyPendingChanges();

        if (poll (&pfds.front(), static_cast<nfds_t> (pfds.size()), 0) == 0)
            return false;

        bool eventWasSent = false;

        for (auto& pfd : pfds)
        {
            if (pfd.revents == 0)
                continue;

            pfd.revents = 0;

            auto fd = pfd.fd;

            for (auto& fdAndCallback : fdReadCallbacks)
            {
                if (fdAndCallback.first == fd)
                {
                    fdAndCallback.second (fd);

                    if (applyPendingChanges())
                    {
                        // elements may have been removed from the fdReadCallbacks/pfds array so we really need
                        // to call poll again
                        return true;
                    }

                    eventWasSent = true;
                }
            }
        }

        return eventWasSent;
    }

    void sleepUntilNextEvent (int timeoutMs)
    {
        poll (&pfds.front(), static_cast<nfds_t> (pfds.size()), timeoutMs);
    }

    std::vector<std::pair<int, std::function<void (int)>>> getFdReadCallbacks()
    {
        const ScopedLock sl (lock);
        return fdReadCallbacks;
    }

    //==============================================================================
    JUCE_DECLARE_SINGLETON (InternalRunLoop, false)

private:
    CriticalSection lock;

    std::vector<std::pair<int, std::function<void (int)>>> fdReadCallbacks;
    std::vector<pollfd> pfds;

    CriticalSection pendingChangesLock;
    std::vector<std::tuple<int, std::function<void (int)>, pollfd>> pendingAdditions;
    std::vector<int> pendingRemovals;
};

JUCE_IMPLEMENT_SINGLETON (InternalRunLoop)

//==============================================================================
namespace LinuxErrorHandling
{
    static bool keyboardBreakOccurred = false;

    void keyboardBreakSignalHandler (int sig)
    {
        if (sig == SIGINT)
            keyboardBreakOccurred = true;
    }

    void installKeyboardBreakHandler()
    {
        struct sigaction saction;
        sigset_t maskSet;
        sigemptyset (&maskSet);
        saction.sa_handler = keyboardBreakSignalHandler;
        saction.sa_mask = maskSet;
        saction.sa_flags = 0;
        sigaction (SIGINT, &saction, nullptr);
    }
}

//==============================================================================
void MessageManager::doPlatformSpecificInitialisation()
{
    if (JUCEApplicationBase::isStandaloneApp())
        LinuxErrorHandling::installKeyboardBreakHandler();

    InternalRunLoop::getInstance();
    InternalMessageQueue::getInstance();
}

void MessageManager::doPlatformSpecificShutdown()
{
    InternalMessageQueue::deleteInstance();
    InternalRunLoop::deleteInstance();
}

bool MessageManager::postMessageToSystemQueue (MessageManager::MessageBase* const message)
{
    if (auto* queue = InternalMessageQueue::getInstanceWithoutCreating())
    {
        queue->postMessage (message);
        return true;
    }

    return false;
}

void MessageManager::broadcastMessage (const String&)
{
    // TODO
}

// this function expects that it will NEVER be called simultaneously for two concurrent threads
bool MessageManager::dispatchNextMessageOnSystemQueue (bool returnIfNoPendingMessages)
{
    for (;;)
    {
        if (LinuxErrorHandling::keyboardBreakOccurred)
            JUCEApplicationBase::quit();

        if (auto* runLoop = InternalRunLoop::getInstanceWithoutCreating())
        {
            if (runLoop->dispatchPendingEvents())
                break;

            if (returnIfNoPendingMessages)
                return false;

            runLoop->sleepUntilNextEvent (2000);
        }
    }

    return true;
}

//==============================================================================
void LinuxEventLoop::registerFdCallback (int fd, std::function<void (int)> readCallback, short eventMask)
{
    if (auto* runLoop = InternalRunLoop::getInstanceWithoutCreating())
        runLoop->registerFdCallback (fd, std::move (readCallback), eventMask);
}

void LinuxEventLoop::unregisterFdCallback (int fd)
{
    if (auto* runLoop = InternalRunLoop::getInstanceWithoutCreating())
        runLoop->unregisterFdCallback (fd);
}

} // namespace juce

JUCE_API std::vector<std::pair<int, std::function<void (int)>>> getFdReadCallbacks()
{
    using namespace juce;

    if (auto* runLoop = InternalRunLoop::getInstanceWithoutCreating())
        return runLoop->getFdReadCallbacks();

    jassertfalse;
    return {};
}
