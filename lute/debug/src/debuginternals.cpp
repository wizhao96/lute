#include "lute/debuginternals.h"

#include "lute/common.h"
#include "lute/require.h"
#include "lute/requirevfs.h"

#include "Luau/Compiler.h"
#include "Luau/DenseHash.h"
#include "Luau/StringUtils.h"

#include "lua.h"
#include "lualib.h"

#include <cstddef>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace debug
{
Breakpoint::Breakpoint(int id, std::string sourcePath, int line, BreakpointStatus status)
    : id(id)
    , sourcePath(sourcePath)
    , line(line)
    , status(status)
{
}

Thread::Thread(int id, std::string name)
    : id(id)
    , name(name)
{
}

bool Thread::operator==(const Thread& other) const
{
    return id == other.id;
}

Target::Target(Runtime& parentRuntime)
    : parentRuntime(parentRuntime)
    , loadedSources("")
{
}

Target::~Target()
{
    // We want to stop the runtime so nothing runs while we are destroying the target but first
    // we need to clear all sources (which are stored as refs in the runtime).
    loadedSources.clear();
    childRuntime.reset();
}

static std::string getChunkFromSource(std::string sourcePath)
{
    return '@' + sourcePath;
}

static std::string getSourceFromChunk(std::string chunkname)
{
    if (chunkname.size() > 0 && chunkname[0] == '@')
        return chunkname.substr(1);
    return chunkname;
}

Breakpoint Target::setBreakpoint(std::string sourcePath, int line)
{
    std::unique_lock lock(targetMutex);
    std::optional<Breakpoint> preexistingBp = getBreakpointBySourceLineHelper(sourcePath, line);
    if (preexistingBp)
        return *preexistingBp;
    int id = currentBreakpointId;
    currentBreakpointId++;
    auto [it, _] = breakpoints.insert_or_assign(id, Breakpoint{id, sourcePath, line, BreakpointStatus::PendingInstall});
    // We schedule breakpoint installs to happen when the runtime exists and we are paused. Otherwise,
    // they are scheduled for pending installs.
    if (childRuntime && paused)
    {
        installBreakpoint(childRuntime->GL, it->second);
        Breakpoint bpCopy = it->second;
        lock.unlock();
        if (bpCopy.status != BreakpointStatus::PendingInstall && launchConfig.onBreakpointInstall)
            launchConfig.onBreakpointInstall(bpCopy);
        return bpCopy;
    }
    return it->second;
}

bool Target::removeBreakpoint(int bpId)
{
    std::unique_lock lock(targetMutex);
    auto it = breakpoints.find(bpId);
    if (it == breakpoints.end())
        return false;
    Breakpoint& bp = it->second;
    if (bp.status == BreakpointStatus::PendingUninstall)
    {
        return false;
    }
    else if (bp.status == BreakpointStatus::Installed)
    {
        // We schedule breakpoint uninstalls to happen async
        if (childRuntime)
        {
            if (!paused)
            {
                bp.status = BreakpointStatus::PendingUninstall;
            }
            else
            {
                bool successful = uninstallBreakpoint(childRuntime->GL, bp);
                if (!successful)
                {
                    return false;
                }
                Breakpoint bpCopy = bp;
                breakpoints.erase(it);
                lock.unlock();
                if (launchConfig.onBreakpointUninstall)
                    launchConfig.onBreakpointUninstall(bpCopy);
            }
        }
        else
        {
            parentRuntime.reporter.reportError(
                Luau::format("breakpoint %d installed at line %d in %s is missing a runtime", bp.id, bp.line, bp.sourcePath.c_str())
            );
        }
    }
    else
    {
        // We can simply erase breakpoints that are pending or invalid
        // since they can never be hit.
        breakpoints.erase(it);
    }
    return true;
}

std::vector<Breakpoint> Target::getBreakpoints() const
{
    std::unique_lock lock(targetMutex);
    std::vector<Breakpoint> all;
    all.reserve(breakpoints.size());
    for (auto& [_, bp] : breakpoints)
        all.emplace_back(bp);
    return all;
}

std::vector<Breakpoint> Target::getBreakpointsByStatus(BreakpointStatus status) const
{
    std::unique_lock lock(targetMutex);
    std::vector<Breakpoint> statusBps;
    statusBps.reserve(breakpoints.size());
    for (auto& [_, bp] : breakpoints)
        if (bp.status == status)
            statusBps.emplace_back(bp);
    return statusBps;
}

std::optional<Breakpoint> Target::getBreakpointByIdHelper(int breakpointId) const
{
    auto it = breakpoints.find(breakpointId);
    if (it != breakpoints.end())
        return it->second;
    return std::nullopt;
}

std::optional<Breakpoint> Target::getBreakpointById(int breakpointId) const
{
    std::unique_lock lock(targetMutex);
    return getBreakpointByIdHelper(breakpointId);
}

std::optional<Breakpoint> Target::getBreakpointBySourceLineHelper(std::string source, int line) const
{
    auto it = std::find_if(
        breakpoints.begin(),
        breakpoints.end(),
        [&source, line](const std::pair<const int, Breakpoint>& entry)
        {
            return entry.second.sourcePath == source && entry.second.line == line;
        }
    );
    if (it == breakpoints.end())
        return std::nullopt;
    return it->second;
}

std::optional<Breakpoint> Target::getBreakpointBySourceLine(std::string source, int line) const
{
    std::unique_lock lock(targetMutex);
    return getBreakpointBySourceLineHelper(source, line);
}

bool Target::installBreakpoint(lua_State* L, Breakpoint& bp)
{
    auto chunkRef = loadedSources.find(bp.sourcePath);
    if (!chunkRef)
        return false;
    (*chunkRef)->push(L);
    int installedLine = lua_breakpoint(L, -1, bp.line, 1);
    lua_pop(L, 1);
    if (installedLine == -1)
    {
        bp.status = BreakpointStatus::Invalid;
        bp.line = -1;
        return false;
    }
    bp.status = BreakpointStatus::Installed;
    bp.line = installedLine;
    return true;
}

// uninstallBreakpoint does not actually remove from the breakpoint map
// to prevent issues with map iteration so callers should remember to do this.
bool Target::uninstallBreakpoint(lua_State* L, Breakpoint& bp)
{
    auto chunkRef = loadedSources.find(bp.sourcePath);
    if (!chunkRef)
    {
        parentRuntime.reporter.reportError(
            Luau::format(
                "breakpoint %d installed at line %d in %s that is queued for uninstall is missing a loaded source",
                bp.id,
                bp.line,
                bp.sourcePath.c_str()
            )
        );
        return false;
    }
    (*chunkRef)->push(L);
    int removed_line = lua_breakpoint(L, -1, bp.line, 0);
    lua_pop(L, 1);
    if (removed_line == -1)
    {
        parentRuntime.reporter.reportError(
            Luau::format(
                "breakpoint %d installed at line %d in %s that is queued for uninstall could not be removed", bp.id, bp.line, bp.sourcePath.c_str()
            )
        );
        return false;
    }
    return true;
}

// This returns the list of installed and uninstalled vectors for use in callbacks
// later.
std::pair<std::vector<Breakpoint>, std::vector<Breakpoint>> Target::modifyPendingBreakpoints(lua_State* L)
{
    std::vector<Breakpoint> installedBpsCallback;
    std::vector<Breakpoint> uninstalledBpsCallback;
    std::vector<int> toErase;
    for (auto& [id, bp] : breakpoints)
    {
        if (bp.status == BreakpointStatus::PendingInstall)
        {
            installBreakpoint(L, bp);
            if (bp.status != BreakpointStatus::PendingInstall && launchConfig.onBreakpointInstall)
                installedBpsCallback.emplace_back(bp);
        }
        if (bp.status == BreakpointStatus::PendingUninstall)
        {
            if (uninstallBreakpoint(L, bp))
            {
                if (launchConfig.onBreakpointUninstall)
                    uninstalledBpsCallback.emplace_back(bp);
                toErase.emplace_back(bp.id);
            }
        }
    }
    for (int id : toErase)
        breakpoints.erase(id);
    return {installedBpsCallback, uninstalledBpsCallback};
}

std::vector<std::string> Target::getLoadedSources()
{
    std::vector<std::string> sources;
    sources.reserve(loadedSources.size());
    for (auto& [path, _] : loadedSources)
    {
        // we discard the empty sentinel value for loadedSources
        if (path != "")
            sources.emplace_back(path);
    }
    return sources;
}

bool Target::launch(const std::string& sourcePath, const std::vector<std::string>& args, LaunchConfig config)
{
    std::vector<Breakpoint> installedBps;
    std::vector<Breakpoint> uninstalledBps;
    {
        std::lock_guard lock(targetMutex);
        // launch() cannot be called twice from the same target.
        if(launched)
            return false;
        childRuntime = std::make_unique<Runtime>(parentRuntime.reporter, true);
        // Set up require system before launch.
        requireCtx = std::make_unique<RequireCtx>(std::make_unique<RequireVfs>());
        requireCtx->compileOptions.optimizationLevel = 1;
        requireCtx->compileOptions.debugLevel = 2;
        requireCtx->onLoad = [this](const std::string& chunkName, lua_State* ML)
        {
            std::unique_lock lock(targetMutex);
            // this strips the potential leading @ from the chunkName for consistency when returning to DAP
            loadedSources[getSourceFromChunk(chunkName)] = std::make_shared<Ref>(ML, -1);
            auto [installed, uninstalled] = modifyPendingBreakpoints(ML);
            lock.unlock();
            for (auto& bp : installed)
                launchConfig.onBreakpointInstall(bp);
            for (auto& bp : uninstalled)
                launchConfig.onBreakpointUninstall(bp);
        };
        setupState(
            *childRuntime,
            [this](lua_State* L)
            {
                luaopen_require(L, requireConfigInit, requireCtx.get());
            }
        );
        launchConfig = config;

        std::ifstream file(sourcePath);
        if (!file.is_open())
        {
            childRuntime.reset();
            return false;
        }
        std::string source((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        Luau::CompileOptions debugOptions = {};
        debugOptions.optimizationLevel = 1;
        debugOptions.debugLevel = 2;
        std::string bytecode = Luau::compile(source, debugOptions);
        lua_State* thread = lua_newthread(childRuntime->GL);
        luaL_sandboxthread(thread);

        std::string chunkname = getChunkFromSource(sourcePath);
        luau_load(thread, chunkname.c_str(), bytecode.c_str(), bytecode.size(), 0);
        loadedSources[sourcePath] = std::make_shared<Ref>(thread, -1);

        std::tie(installedBps, uninstalledBps) = modifyPendingBreakpoints(thread);
        for (const std::string& arg : args)
            lua_pushstring(thread, arg.c_str());
        childRuntime->runningThreads.push_back({true, getRefForThread(thread), static_cast<int>(args.size())});
        lua_pop(childRuntime->GL, 1);

        // thread initialization
        scriptThread = thread;
        threadIdToState[threadId] = thread;
        stateToThread[thread] = Thread(threadId, "Thread " + std::to_string(threadId));
        threadId++;

        // install user data before all callbacks
        lua_Callbacks* cb = lua_callbacks(childRuntime->GL);
        cb->userdata = this;
        installBpHitCallback();
        installExitCallback();
        installThreadCallback();

        // All VM setup happens synchronously before runContinuously starts the background thread.
        // The no-op schedule wakes the event loop so it picks up the queued thread.
        paused = false;
        launched = true;
        childRuntime->schedule([]() {});
        childRuntime->runContinuously();
    }
    for (auto& bp : installedBps)
        launchConfig.onBreakpointInstall(bp);
    for (auto& bp : uninstalledBps)
        launchConfig.onBreakpointUninstall(bp);
    return true;
}

void Target::installBpHitCallback()
{
    lua_Callbacks* cb = lua_callbacks(childRuntime->GL);
    cb->debugbreak = [](lua_State* L, lua_Debug* ar)
    {
        auto target = static_cast<Target*>(lua_callbacks(L)->userdata);
        // We land on the same instruction after a continue() after hitting a bp so we basically don't do anything
        std::unique_lock lock(target->targetMutex);
        if (auto it = target->continueRequestedBp.find(L); it != target->continueRequestedBp.end())
        {
            target->continueRequestedBp.erase(it);
            return;
        }
        lua_Debug info = {};
        lua_getinfo(L, 0, "sl", &info);
        int line = info.currentline;
        if (!info.source)
        {
            target->parentRuntime.reporter.reportError(Luau::format("breakpoint hit at line %d could not find a runtime source", line));
            return;
        }
        target->stoppedBpLine = line;
        std::string chunkname = info.source;
        std::optional<Breakpoint> bp = target->getBreakpointBySourceLineHelper(getSourceFromChunk(chunkname), line);
        // Only stop execution on installed breakpoints; otherwise, don't stop.
        if (bp && bp->status == BreakpointStatus::Installed)
        {
            target->bpHit = *bp;
            target->paused = true;
            target->childRuntime->stopDebug();
            target->stoppedThread = L;
            lua_break(L);
            auto [installed, uninstalled] = target->modifyPendingBreakpoints(target->scriptThread);
            debug::Thread thread = target->stateToThread[L];
            lock.unlock();
            if (target->launchConfig.onBreakpointHit)
                target->launchConfig.onBreakpointHit(thread, bp.value());
            for (auto& bp : installed)
                target->launchConfig.onBreakpointInstall(bp);
            for (auto& bp : uninstalled)
                target->launchConfig.onBreakpointUninstall(bp);
        }
        else if (!bp || bp->status != BreakpointStatus::PendingUninstall)
        {
            // It is normal to hit breakpoints that are pending uninstall but not normal
            // to hit any other type of breakpoint so we error in those cases.
            target->parentRuntime.reporter.reportError(
                Luau::format("breakpoint hit at line %d in %s could not be found in breakpoint map", line, chunkname.c_str())
            );
        }
    };
}

// When the main script's coroutine exits, we can say that our debugging of the debuggee has terminated.
// Theoretically, the debuggee can still be alive on the Runtime but should be cleaned up after
// the Target itself is destroyed.
void Target::installExitCallback()
{
    ThreadCompletionHandler completion;
    completion.onFinish = [this](lua_State* L, int status)
    {
        if (launchConfig.onExit)
            launchConfig.onExit(status == LUA_OK);
    };
    childRuntime->addThreadCompletionHandler(scriptThread, std::move(completion));
}

void Target::installThreadCallback()
{
    lua_Callbacks* cb = lua_callbacks(childRuntime->GL);
    cb->userthread = [](lua_State* LP, lua_State* L)
    {
        auto target = static_cast<Target*>(lua_callbacks(L)->userdata);
        std::unique_lock lock(target->targetMutex);

        // this means a thread is being garbage collected
        if (LP == nullptr)
        {
            if (auto it = target->stateToThread.find(L); it != target->stateToThread.end())
            {
                int id = it->second.id;
                target->stateToThread.erase(it);
                if (auto it2 = target->threadIdToState.find(id); it2 != target->threadIdToState.end())
                {
                    target->threadIdToState.erase(it2);
                }
                else
                {
                    target->parentRuntime.reporter.reportError(Luau::format("userthread callback fired for unregistered thread id %d", id));
                }
            }
            else
            {
                target->parentRuntime.reporter.reportError(Luau::format("userthread callback fired for unregistered lua_State* %p", (void*)L));
            }
        }
        else
        {
            target->threadIdToState[target->threadId] = L;
            target->stateToThread[L] = Thread(target->threadId, "Thread " + std::to_string(target->threadId));
            target->threadId++;
        }
    };
}

std::vector<Thread> Target::getThreads() const
{
    std::unique_lock lock(targetMutex);
    std::vector<Thread> result;
    for (auto& [L, thread] : stateToThread)
    {
        if (lua_costatus(childRuntime->GL, L) != LUA_COFIN && lua_costatus(childRuntime->GL, L) != LUA_COERR)
            result.push_back(thread);
    }
    return result;
}


std::optional<StackFrame> Target::getStackFrameHelper(int threadId, int level)
{
    if (!paused)
        return std::nullopt;
    if (threadIdToState.find(threadId) == threadIdToState.end())
    {
        return std::nullopt;
    }
    std::unordered_map<int, StackFrame>& levelMap = stateToStackFrame[threadId];
    auto it = levelMap.find(level);
    if (it == levelMap.end())
    {
        StackFrame frame;
        frame.id = stackframeId;
        stackframeId++;
        lua_Debug ar = {};
        if (!lua_getinfo(threadIdToState[threadId], level, "sln", &ar))
            return std::nullopt;

        frame.name = ar.name ? ar.name : "(anonymous)";
        if (ar.source)
        {
            frame.sourcePath = getSourceFromChunk(ar.source);
            // edge case: when we hit a breakpoint, the pc is sent backward one
            // so that we can hit it again, so lua_getinfo() fails.
            if (level == 0 && bpHit)
                frame.line = stoppedBpLine;
            else
                frame.line = ar.currentline;
        }
        else
        {
            frame.sourcePath = "";
            frame.line = 0;
        }
        frame.column = 0;
        levelMap[level] = frame;
        idToStackFrameInfo[frame.id] = std::make_pair(threadId, level);
        return frame;
    }
    return it->second;
}

std::optional<StackFrame> Target::getStackFrame(int threadId, int level)
{
    std::unique_lock lock(targetMutex);
    return getStackFrameHelper(threadId, level);
}

std::optional<std::vector<StackFrame>> Target::getStackTrace(int threadId, int startLevel, int numFrames)
{
    std::unique_lock lock(targetMutex);
    if (!paused)
        return std::nullopt;
    if (threadIdToState.find(threadId) == threadIdToState.end())
    {
        return std::nullopt;
    }
    int stackDepth = lua_stackdepth(threadIdToState[threadId]);
    if (startLevel >= stackDepth)
    {
        return std::nullopt;
    }
    int maximumLevel;
    if (numFrames == 0)
    {
        maximumLevel = stackDepth;
    }
    else
    {
        maximumLevel = std::min(startLevel + numFrames, stackDepth);
    }
    std::vector<StackFrame> stackTrace;
    for (int i = startLevel; i < maximumLevel; i++)
    {
        std::optional<StackFrame> frame = getStackFrameHelper(threadId, i);
        if (!frame)
        {
            return std::nullopt;
        }
        stackTrace.emplace_back(*frame);
    }
    return stackTrace;
}

bool Target::continueProcess()
{

    std::unique_lock lock(targetMutex);
    if (!launched || !paused)
        return false;
    // this clears the interrupts that triggers when the process is paused from client request
    // in case it has not actually been triggered.
    lua_Callbacks* cb = lua_callbacks(childRuntime->GL);
    cb->interrupt = nullptr;
    if (stoppedThread)
    {
        // we are continuing on a breakpoint and so might need to flag continueRequestedBp.
        if (bpHit)
        {
            // we need to check if our breakpoint is still currently installed after
            // onBreakpointHit() callback
            std::optional<Breakpoint> currentBp = getBreakpointByIdHelper(bpHit->id);
            if (currentBp && currentBp->status == BreakpointStatus::Installed)
                continueRequestedBp.insert(stoppedThread);
            bpHit = std::nullopt;
            stoppedBpLine = -1;
        }
        childRuntime->runningThreads.push_back({true, getRefForThread(stoppedThread), 0});
        // This schedule() wakes up the runtime in runContinuously() to re-run runToCompletion() in case that has exited. This is a no-op if
        // runToCompletion() has not exited.
        childRuntime->schedule([]() {});
        stoppedThread = nullptr;
    }
    // we clear the stack frame information
    stackframeId = 0;
    stateToStackFrame.clear();
    idToStackFrameInfo.clear();
    paused = false;
    childRuntime->continueDebug();
    return true;
}

bool Target::pauseProcess()
{
    std::unique_lock lock(targetMutex);
    if (!launched || paused)
        return false;
    lua_Callbacks* cb = lua_callbacks(childRuntime->GL);
    cb->userdata = this;
    // the interrupt callback calls at any safepoint, which
    // is the soonest we can pause execution safely.
    // safepoints are loop back edges or function calls/returns.
    cb->interrupt = [](lua_State* L, int gc)
    {
        // gc runs when it is not -1
        if (gc != -1)
            return;
        auto target = static_cast<Target*>(lua_callbacks(L)->userdata);
        std::unique_lock lock(target->targetMutex);
        target->paused = true;
        target->childRuntime->stopDebug();
        target->stoppedThread = L;
        debug::Thread thread = target->stateToThread[L];
        // We transition into a paused state. Let's modify all pending breakpoints.
        auto [installed, uninstalled] = target->modifyPendingBreakpoints(target->scriptThread);
        lua_break(L);
        // Clear out the interrupt callback after we are done.
        lua_callbacks(L)->interrupt = nullptr;
        lock.unlock();
        // Since pausing actually only happens when the interrupt callback runs we have a callback
        if (target->launchConfig.onPause)
            target->launchConfig.onPause(thread);
        for (auto& bp : installed)
            target->launchConfig.onBreakpointInstall(bp);
        for (auto& bp : uninstalled)
            target->launchConfig.onBreakpointUninstall(bp);
    };
    return true;
}

} // namespace debug
