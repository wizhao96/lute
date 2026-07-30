#include "lute/debuginternals.h"

#include "lute/common.h"
#include "lute/require.h"
#include "lute/requirevfs.h"

#include "Luau/Compiler.h"
#include "Luau/DenseHash.h"
#include "Luau/FileUtils.h"
#include "Luau/StringUtils.h"

#include "lua.h"
#include "lualib.h"

#include <cmath>
#include <cstddef>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

#include "lstate.h"

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


VariableScope::VariableScope(int variableReference, VariableScopeType type, std::string name, int threadId, int level, int luaref)
    : variableReference(variableReference)
    , type(type)
    , name(name)
    , threadId(threadId)
    , level(level)
    , luaref(luaref)
{
}

VariableScope VariableScope::makeLocals(int variableReference, int threadId, int level)
{
    return VariableScope(variableReference, VariableScopeType::Locals, "Locals", threadId, level, -1);
}

VariableScope VariableScope::makeUpvalues(int variableReference, int threadId, int level)
{
    return VariableScope(variableReference, VariableScopeType::Upvalues, "Upvalues", threadId, level, -1);
}

VariableScope VariableScope::makeTable(int variableReference, int luaref)
{
    return VariableScope(variableReference, VariableScopeType::Table, "Table", -1, -1, luaref);
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
    std::unique_lock lock(targetMutex);
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

int Target::getLine()
{
    std::unique_lock lock(targetMutex);
    if (!launched || !paused)
        return -1;
    return stoppedLine;
}

void Target::computeStoppedLine(lua_State* L)
{
    lua_Debug info = {};
    lua_getinfo(L, 0, "l", &info);
    stoppedLine = info.currentline;
}

bool Target::launch(std::string sourcePath, const std::vector<std::string>& args, LaunchConfig config)
{
    std::vector<Breakpoint> installedBps;
    std::vector<Breakpoint> uninstalledBps;
    {
        std::scoped_lock lock(targetMutex);
        sourcePath = normalizePath(sourcePath);
        // launch() cannot be called twice from the same target, so we assert in
        // debug mode and return false when we are in release mode.
        LUTE_ASSERT(!launched);
        if (launched)
            return false;
        childRuntime = std::make_unique<Runtime>(parentRuntime.reporter, true);
        // Set up require system before launch.
        Luau::CompileOptions debugOptions;
        debugOptions.optimizationLevel = 1;
        debugOptions.debugLevel = 2;
        std::function<void(lua_State * L, const std::string& chunkName)> onChunkLoad = [this](lua_State* ML, const std::string& chunkName)
        {
            std::string source = getSourceFromChunk(chunkName);
            std::vector<Breakpoint> installed;
            std::vector<Breakpoint> uninstalled;
            {
                std::scoped_lock lock(targetMutex);
                // this strips the potential leading @ from the chunkName for consistency when returning to DAP
                loadedSources[source] = std::make_shared<Ref>(ML, -1);
                std::tie(installed, uninstalled) = modifyPendingBreakpoints(ML);
            }
            for (auto& bp : installed)
                launchConfig.onBreakpointInstall(bp);
            for (auto& bp : uninstalled)
                launchConfig.onBreakpointUninstall(bp);
        };
        requireCtx = std::make_unique<RequireCtx>(std::make_unique<RequireVfs>(), debugOptions, onChunkLoad);
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
        std::string bytecode = Luau::compile(source, debugOptions);
        lua_State* thread = lua_newthread(childRuntime->GL);
        luaL_sandboxthread(thread);

        std::string chunkname = getChunkFromSource(sourcePath);
        // TODO: surface compilation errors to the user when debugging.
        if (luau_load(thread, chunkname.c_str(), bytecode.c_str(), bytecode.size(), 0) != 0)
        {
            childRuntime.reset();
            return false;
        }
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
        lua_getinfo(L, 0, "s", &info);
        target->stoppedLine = ar->currentline;
        int line = ar->currentline;
        if (!info.source)
        {
            target->parentRuntime.reporter.reportError(Luau::format("breakpoint hit at line %d could not find a runtime source", line));
            return;
        }
        target->stoppedLine = line;
        target->stoppedPc = L->ci->savedpc;
        std::string chunkname = info.source;
        std::optional<Breakpoint> bp = target->getBreakpointBySourceLineHelper(getSourceFromChunk(chunkname), line);
        // Only stop execution on installed breakpoints; otherwise, don't stop.
        if (bp && bp->status == BreakpointStatus::Installed)
        {
            target->bpHit = *bp;
            target->paused = true;
            target->childRuntime->stopDebug();
            target->stoppedThread = L;
            // Clear out stepping when this happens.
            lua_callbacks(L)->debugstep = nullptr;
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
            result.emplace_back(thread);
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
            if (level == 0 && stoppedLine != -1)
                frame.line = stoppedLine;
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

std::optional<std::vector<VariableScope>> Target::getScopesHelper(int threadId, int level)
{
    std::optional<StackFrame> frame = getStackFrameHelper(threadId, level);
    if (!frame)
        return std::nullopt;
    if (scopeCache.find(frame->id) != scopeCache.end())
        return scopeCache[frame->id];
    std::vector<VariableScope> contexts;
    VariableScope locals = VariableScope::makeLocals(variableRefId, threadId, level);
    variableContexts.insert_or_assign(variableRefId, locals);
    variableRefId++;
    contexts.push_back(locals);
    VariableScope upvalues = VariableScope::makeUpvalues(variableRefId, threadId, level);
    variableContexts.insert_or_assign(variableRefId, upvalues);
    variableRefId++;
    contexts.push_back(upvalues);
    scopeCache[frame->id] = contexts;
    return contexts;
}

std::optional<std::vector<VariableScope>> Target::getScopes(int frameId)
{
    std::unique_lock lock(targetMutex);
    if (!paused)
        return std::nullopt;
    auto it = idToStackFrameInfo.find(frameId);
    if (it == idToStackFrameInfo.end())
        return std::nullopt;
    auto [threadId, level] = it->second;
    return getScopesHelper(threadId, level);
}

static std::string convertNumberToString(lua_State* L, int stackSlot)
{
    double val = lua_tonumber(L, stackSlot);
    if (val == trunc(val))
        return std::to_string(lua_tointeger(L, stackSlot));
    char buf[64];
    snprintf(buf, sizeof(buf), "%.15g", val);
    return buf;
}

// We assume that the value is at position -1 on the stack and the key is at position -2.
static std::string getKeyFromTableType(lua_State* L)
{
    std::string key;
    switch (lua_type(L, -2))
    {
    case LUA_TSTRING:
    {
        key = std::string(lua_tostring(L, -2));
        break;
    }
    case LUA_TNUMBER:
        key = "[" + convertNumberToString(L, -2) + "]";
        break;
    case LUA_TBOOLEAN:
        key = lua_toboolean(L, -2) ? "[true]" : "[false]";
        break;
    case LUA_TTABLE:
        key = "[table]";
        break;
    default:
        key = "[" + std::string(lua_typename(L, lua_type(L, -2))) + "]";
        break;
    }
    return key;
}

// Gets all variables from a table at index idx of the stack
static std::string printTable(lua_State* L, int idx, int levelsToPrint)
{
    if (levelsToPrint <= 0)
        return "{...}";
    int absoluteIndex = lua_absindex(L, idx);
    std::string result = "{";
    lua_pushnil(L);
    std::vector<std::pair<std::string, std::string>> keyValues;
    while (lua_next(L, absoluteIndex))
    {
        std::string key = getKeyFromTableType(L);
        std::string value;
        switch (lua_type(L, -1))
        {
        case LUA_TNUMBER:
            value = convertNumberToString(L, -1);
            break;
        case LUA_TSTRING:
            value = "\"" + std::string(lua_tostring(L, -1)) + "\"";
            break;
        case LUA_TBOOLEAN:
            value = lua_toboolean(L, -1) ? "true" : "false";
            break;
        case LUA_TTABLE:
            value = printTable(L, -1, levelsToPrint - 1);
            break;
        default:
            value = lua_typename(L, lua_type(L, -1));
            break;
        }
        keyValues.push_back(std::make_pair(key, value));
        lua_pop(L, 1);
    }
    // this is a "pure" array
    if ((int)(keyValues.size()) == lua_objlen(L, idx))
    {
        for (auto [_, value] : keyValues)
        {
            if (result != "{")
                result += ", ";
            result += value;
        }
    }
    else
    {
        for (auto [key, value] : keyValues)
        {
            if (result != "{")
                result += ", ";
            result += key + "=" + value;
        }
    }
    return result + "}";
}

// Turns the object on the top of the stack into a variable object.
Variable Target::makeVariable(lua_State* L, const std::string& name)
{
    Variable var;
    var.name = name;
    var.type = lua_typename(L, lua_type(L, -1));
    switch (lua_type(L, -1))
    {
    case LUA_TNUMBER:
    {
        var.value = convertNumberToString(L, -1);
        break;
    }
    case LUA_TSTRING:
        var.value = "\"" + std::string(lua_tostring(L, -1)) + "\"";
        break;
    case LUA_TBOOLEAN:
        var.value = lua_toboolean(L, -1) ? "true" : "false";
        break;
    case LUA_TTABLE:
    {
        var.value = printTable(L, -1, 2);
        var.variableReference = variableRefId;
        lua_pushvalue(L, -1);
        int ref = lua_ref(L, -1);
        lua_pop(L, 1);
        variableContexts.insert_or_assign(variableRefId, VariableScope::makeTable(variableRefId, ref));
        variableRefId++;
        break;
    }
    default:
        var.value = lua_typename(L, lua_type(L, -1));
        break;
    }
    return var;
}

// Gets all local variables of a lua_State* and its frame at a certain level, given that
// such a frame exists.
std::vector<Variable> Target::getLocalsHelper(lua_State* L, int level)
{
    // when hitting a bp we try to re-enter
    bool fixedSavedpc = false;
    const Instruction* original = L->ci->savedpc;
    if (level == 0 && L == stoppedThread && L->ci)
    {
        L->ci->savedpc = stoppedPc;
        fixedSavedpc = true;
    }
    const char* name;
    int n = 1;
    std::vector<Variable> vars;
    while (true)
    {
        name = lua_getlocal(L, level, n);
        if (name == nullptr)
            break;
        vars.push_back(makeVariable(L, name));
        lua_pop(L, 1);
        n++;
    }
    if (fixedSavedpc)
        L->ci->savedpc = original;
    return vars;
}

// Gets all upvalues of a lua_State* and it stack frame ata certain level, given
// that such a frame exists
std::vector<Variable> Target::getUpvaluesHelper(lua_State* L, int level)
{
    std::vector<Variable> vars;
    lua_Debug ar = {};
    lua_getinfo(L, level, "f", &ar);
    int n = 1;
    const char* name;
    while (true)
    {
        name = lua_getupvalue(L, -1, n);
        if (name == nullptr)
            break;
        Variable var = makeVariable(L, name);
        lua_pop(L, 1);
        vars.push_back(var);
        n++;
    }
    lua_pop(L, 1);
    return vars;
}

// Gets all variables from a table at index idx of the stack
std::vector<Variable> Target::getTableHelper(lua_State* L, int idx)
{
    std::vector<Variable> vars;
    int absoluteIndex = lua_absindex(L, idx);
    lua_pushnil(L);
    while (lua_next(L, absoluteIndex))
    {
        std::string key = getKeyFromTableType(L);
        vars.push_back(makeVariable(L, key));
        lua_pop(L, 1);
    }
    return vars;
}


std::optional<std::vector<Variable>> Target::getVariablesHelper(int varRef)
{
    auto it = variableContexts.find(varRef);
    if (it == variableContexts.end())
    {
        return std::nullopt;
    }
    VariableScope context = it->second;
    if (auto it2 = variableCache.find(varRef); it2 != variableCache.end())
    {
        return it2->second;
    }
    std::vector<Variable> vars;
    if (context.type == VariableScopeType::Locals)
    {
        vars = getLocalsHelper(threadIdToState[context.threadId], context.level);
    }
    else if (context.type == VariableScopeType::Upvalues)
    {
        vars = getUpvaluesHelper(threadIdToState[context.threadId], context.level);
    }
    else
    {
        lua_rawgeti(childRuntime->GL, LUA_REGISTRYINDEX, context.luaref);
        vars = getTableHelper(childRuntime->GL, -1);
        lua_pop(childRuntime->GL, 1);
    }
    variableCache[varRef] = vars;
    return vars;
}

std::optional<std::vector<Variable>> Target::getVariables(int varRef)
{
    std::unique_lock lock(targetMutex);
    if (!paused)
    {
        return std::nullopt;
    }
    return getVariablesHelper(varRef);
}

std::optional<std::vector<Variable>> Target::getVariablesByScopeType(int frameId, VariableScopeType contextType)
{
    std::unique_lock lock(targetMutex);
    if (!paused)
        return std::nullopt;
    auto stackFrame = idToStackFrameInfo.find(frameId);
    if (stackFrame == idToStackFrameInfo.end())
        return std::nullopt;
    auto [threadId, level] = stackFrame->second;
    std::optional<std::vector<VariableScope>> scopes = getScopesHelper(threadId, level);
    if (!scopes)
        return std::nullopt;
    auto it = std::find_if(
        scopes->begin(),
        scopes->end(),
        [&](const VariableScope& ctx)
        {
            return ctx.type == contextType;
        }
    );
    if (it == scopes->end())
    {
        return std::nullopt;
    }
    return getVariablesHelper(it->variableReference);
}

void Target::continueProcessHelper(bool isStepping)
{
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
        }
        stoppedPc = nullptr;
        stoppedLine = -1;
        childRuntime->runningThreads.push_back({true, getRefForThread(stoppedThread), 0});
        // This schedule() wakes up the runtime in runContinuously() to re-run runToCompletion() in case that has exited. This is a no-op if
        // runToCompletion() has not exited.
        childRuntime->schedule([]() {});
        if (!isStepping)
            stoppedThread = nullptr;
    }
    // this clears the interrupts that triggers when the process is paused from client request
    // in case it has not actually been triggered.
    lua_Callbacks* cb = lua_callbacks(childRuntime->GL);
    cb->interrupt = nullptr;

    // we clear the stack frame information
    stackframeId = 0;
    stateToStackFrame.clear();
    idToStackFrameInfo.clear();

    variableRefId = 1;
    variableContexts.clear();
    variableCache.clear();
    scopeCache.clear();

    paused = false;
    childRuntime->continueDebug();
}

bool Target::continueProcess()
{
    std::unique_lock lock(targetMutex);
    if (!launched || !paused)
        return false;
    continueProcessHelper(false);
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
        // We transition into a paused state. Let's modify all pending breakpoints.
        auto [installed, uninstalled] = target->modifyPendingBreakpoints(target->scriptThread);
        target->computeStoppedLine(L);
        target->stoppedPc = L->ci->savedpc;
        lua_break(L);
        // Clear out the interrupt and debugstep callback after we are done.
        lua_callbacks(L)->interrupt = nullptr;
        Thread thread = target->stateToThread[L];
        lua_callbacks(L)->debugstep = nullptr;
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

bool Target::step(int threadId, StepType type)
{
    std::unique_lock lock(targetMutex);
    if (!launched || !paused)
        return false;
    if (threadIdToState.find(threadId) == threadIdToState.end())
        return false;
    lua_State* stepThread = threadIdToState[threadId];
    Thread info = stateToThread[stepThread];
    int startLine = stoppedLine, startDepth = lua_stackdepth(stepThread);
    stepInfo = {info, type, startLine, startDepth};
    lua_singlestep(stepThread, 1);
    lua_Callbacks* cb = lua_callbacks(childRuntime->GL);
    cb->debugstep = [](lua_State* L, lua_Debug* ar)
    {
        auto target = static_cast<Target*>(lua_callbacks(L)->userdata);
        std::unique_lock lock(target->targetMutex);
        if (!target->stepInfo)
        {
            target->parentRuntime.reporter.reportError(Luau::format("target lacks stepping info even while stepping at line %d", ar->currentline));
            return;
        }
        bool stopStepping = false;
        StepInfo stepInfo = *target->stepInfo;
        if (target->threadIdToState.find(stepInfo.thread.id) == target->threadIdToState.end())
        {
            target->parentRuntime.reporter.reportError(Luau::format("could not finding stepping thread %d in thread map", stepInfo.thread.id));
            return;
        }
        lua_State* steppingThread = target->threadIdToState[stepInfo.thread.id];
        if (L != steppingThread)
            return;
        int line = ar->currentline;
        int depth = lua_stackdepth(L);
        switch (stepInfo.type)
        {
        case StepType::StepIn:
            if (line != stepInfo.startLine || depth != stepInfo.startDepth)
            {
                stopStepping = true;
            }
            break;
        case StepType::StepOver:
            if (depth <= stepInfo.startDepth && line != stepInfo.startLine)
            {
                stopStepping = true;
            }
            break;
        case StepType::StepOut:
            if (depth < stepInfo.startDepth)
            {
                stopStepping = true;
            }
            break;
        }
        if (stopStepping)
        {
            target->paused = true;
            target->childRuntime->stopDebug();
            target->stoppedThread = L;
            target->stoppedLine = ar->currentline;
            target->stoppedPc = L->ci->savedpc;
            target->stepInfo = std::nullopt;
            auto [installed, uninstalled] = target->modifyPendingBreakpoints(target->scriptThread);
            lua_break(L);
            lua_singlestep(L, 0);
            lua_callbacks(L)->debugstep = nullptr;
            Thread thread = target->stateToThread[L];
            lock.unlock();
            // Since pausing actually only happens when the step callback runs we have a callback
            if (target->launchConfig.onStepStop)
                target->launchConfig.onStepStop(thread, stepInfo);
            for (auto& bp : installed)
                target->launchConfig.onBreakpointInstall(bp);
            for (auto& bp : uninstalled)
                target->launchConfig.onBreakpointUninstall(bp);
        }
        else
        {
            return;
        }
    };
    continueProcessHelper(true);
    return true;
}

bool Target::stepIn(int threadId)
{
    return step(threadId, StepType::StepIn);
}

bool Target::stepOver(int threadId)
{
    return step(threadId, StepType::StepOver);
}

bool Target::stepOut(int threadId)
{
    return step(threadId, StepType::StepOut);
}

} // namespace debug
