#include "lute/debuginternals.h"

#include <chrono>
#include <future>
#include <thread>
#include <vector>

#include "debugfixture.h"
#include "doctest.h"

using namespace debug;

static void checkBreakpoint(Target& target, int id, BreakpointStatus status, int line)
{
    auto foundBp = target.getBreakpointById(id);
    REQUIRE(foundBp.has_value());
    CHECK(foundBp->id == id);
    CHECK(foundBp->status == status);
    CHECK(foundBp->line == line);
}

static void checkScope(const std::vector<VariableScope>& scopes, VariableScopeType type, std::string name, int threadId, int level)
{
    auto foundScope = std::find_if(
        scopes.begin(),
        scopes.end(),
        [&](const VariableScope& scope)
        {
            return scope.name == name;
        }
    );
    REQUIRE(foundScope != scopes.end());
    CHECK(foundScope->type == type);
    CHECK(foundScope->threadId == threadId);
    CHECK(foundScope->level == level);
    CHECK(foundScope->variableReference > 0);
}

static int checkVariable(const std::vector<Variable>& vars, const std::string& name, const std::string& value, const std::string& type, bool isTable)
{
    auto foundVar = std::find_if(
        vars.begin(),
        vars.end(),
        [&](const Variable& v)
        {
            return v.name == name;
        }
    );
    REQUIRE(foundVar != vars.end());
    CHECK(foundVar->value == value);
    CHECK(foundVar->type == type);
    if (isTable)
    {
        CHECK(foundVar->variableReference > 0);
    }
    else
    {
        CHECK(foundVar->variableReference == 0);
    }
    return foundVar->variableReference;
}

TEST_SUITE("Debug")
{
    TEST_CASE_FIXTURE(DebugFixture, "Debug_setBreakpoint")
    {
        std::string fixturePath = getDebugFixturePath("simple.luau");
        Target target(*runtime);

        // valid breakpoint
        Breakpoint bp = target.setBreakpoint(fixturePath, 2);
        // invalid breakpoint
        Breakpoint bp2 = target.setBreakpoint(fixturePath, 100);
        // breakpoint that will be moved on installation due to whitespace
        Breakpoint bp3 = target.setBreakpoint(fixturePath, 3);

        // check breakpoints before launch
        CHECK(bp.id == 0);
        checkBreakpoint(target, bp.id, BreakpointStatus::PendingInstall, 2);
        CHECK(bp2.id == 1);
        checkBreakpoint(target, bp2.id, BreakpointStatus::PendingInstall, 100);
        CHECK(bp3.id == 2);
        checkBreakpoint(target, bp3.id, BreakpointStatus::PendingInstall, 3);

        // check cannot find not-set breakpoint
        std::optional<Breakpoint> found = target.getBreakpointById(999);
        REQUIRE(!found.has_value());

        std::promise<void> bp4Promise;
        std::future<void> bp4Future = bp4Promise.get_future();
        std::function<void(const Breakpoint& bp)> onBreakpointInstall = [&](const Breakpoint& bp)
        {
            if (bp.id == 3)
                bp4Promise.set_value();
        };

        std::function<void(const Thread&, const Breakpoint& bp)> onBreakpointHit = [&](const Thread&, const Breakpoint& bp)
        {
            bool continuedProcess = target.continueProcess();
            CHECK(continuedProcess);
        };

        config.onBreakpointInstall = onBreakpointInstall;
        config.onBreakpointHit = onBreakpointHit;

        bool launched = target.launch(fixturePath, {}, config);
        CHECK(launched);

        // check breakpoints before launch are immediately installed after launch
        CHECK(target.getBreakpoints().size() == 3);
        CHECK(target.getBreakpointsByStatus(BreakpointStatus::PendingInstall).size() == 0);
        CHECK(target.getBreakpointsByStatus(BreakpointStatus::Installed).size() == 2);
        CHECK(target.getBreakpointsByStatus(BreakpointStatus::Invalid).size() == 1);
        checkBreakpoint(target, bp.id, BreakpointStatus::Installed, 2);
        checkBreakpoint(target, bp2.id, BreakpointStatus::Invalid, -1);
        checkBreakpoint(target, bp3.id, BreakpointStatus::Installed, 4);

        // check that adding breakpoints after launch should be installed at some point
        Breakpoint bp4 = target.setBreakpoint(fixturePath, 1);
        REQUIRE(bp4Future.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
        checkBreakpoint(target, bp4.id, BreakpointStatus::Installed, 1);

        // check that setting breakpoints at same breakpoint returns same id
        Breakpoint bp5 = target.setBreakpoint(fixturePath, 2);
        CHECK(bp5.id == 0);
        checkBreakpoint(target, bp5.id, BreakpointStatus::Installed, 2);

        // wait until script is finished to call destructors
        REQUIRE(exitFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    }

    TEST_CASE_FIXTURE(DebugFixture, "Debug_removeBreakpoint")
    {
        std::string fixturePath = getDebugFixturePath("simple.luau");
        Target target(*runtime);

        // check removing pending breakpoint
        Breakpoint bp = target.setBreakpoint(fixturePath, 2);
        CHECK(target.getBreakpoints().size() == 1);
        checkBreakpoint(target, bp.id, BreakpointStatus::PendingInstall, 2);
        bool removedBp1 = target.removeBreakpoint(bp.id);
        CHECK(removedBp1);
        REQUIRE(!target.getBreakpointById(bp.id).has_value());
        CHECK(target.getBreakpoints().size() == 0);

        // check removing installed breakpoints and invalid breakpoints
        Breakpoint bp2 = target.setBreakpoint(fixturePath, 3);
        Breakpoint bp3 = target.setBreakpoint(fixturePath, 100);
        CHECK(target.getBreakpoints().size() == 2);

        int numUninstalledBps = 0;
        std::promise<void> bp2Promise;
        std::future<void> bp2Future = bp2Promise.get_future();
        std::function<void(const Breakpoint& bp)> onBreakpointUninstall = [&](const Breakpoint& bp)
        {
            numUninstalledBps++;
            if (bp.id == bp2.id)
                bp2Promise.set_value();
        };

        // trigger breakpoint removals when our breakpoint is hit (thus, the execution is currently paused
        // and we can see changes to it being pending uninstall)
        std::function<void(const Thread&, const Breakpoint& bp)> onBreakpointHit = [&](const Thread&, const Breakpoint& bp)
        {
            checkBreakpoint(target, bp.id, BreakpointStatus::Installed, bp.line);

            bool removedBp2 = target.removeBreakpoint(bp.id);
            CHECK(removedBp2);
            REQUIRE(!target.getBreakpointById(bp.id).has_value());

            bool continuedProcess = target.continueProcess();
            CHECK(continuedProcess);
        };

        config.onBreakpointUninstall = onBreakpointUninstall;
        config.onBreakpointHit = onBreakpointHit;
        bool launched = target.launch(fixturePath, {}, config);
        CHECK(launched);

        checkBreakpoint(target, bp3.id, BreakpointStatus::Invalid, -1);
        // check invalid breakpoint is installed instantenously
        bool removedBp3 = target.removeBreakpoint(bp3.id);
        CHECK(removedBp3);
        CHECK(!target.getBreakpointById(bp3.id).has_value());

        // check installed breakpoint is actually uninstalled by hitting the breakpoint
        REQUIRE(bp2Future.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
        CHECK(numUninstalledBps == 1);
        CHECK(!target.getBreakpointById(bp2.id).has_value());

        // check cannot remove never added breakpoint
        bool cannotRemove = target.removeBreakpoint(100);
        CHECK(!cannotRemove);

        // wait until script is finished to call destructors
        REQUIRE(exitFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    }

    TEST_CASE_FIXTURE(DebugFixture, "Debug_hitBreakpoint")
    {
        std::string fixturePath = getDebugFixturePath("loop.luau");
        Target target(*runtime);

        Breakpoint bp1 = target.setBreakpoint(fixturePath, 4);
        Breakpoint bp2 = target.setBreakpoint(fixturePath, 6);
        // bp3 checks that removing a breakpoint makes sure that breakpoint is never hit again
        Breakpoint bp3 = target.setBreakpoint(fixturePath, 7);
        int hitBp1 = 0, hitBp2 = 0, hitBp3 = 0;

        std::function<void(const Thread&, const Breakpoint& bp)> onBreakpointHit = [&](const Thread&, const Breakpoint& hitBp)
        {
            if (hitBp.id == bp1.id)
                hitBp1++;
            if (hitBp.id == bp2.id)
                hitBp2++;
            if (hitBp.id == bp3.id)
            {
                hitBp3++;
                target.removeBreakpoint(bp3.id);
            }
            target.continueProcess();
        };

        config.onBreakpointHit = onBreakpointHit;
        target.launch(fixturePath, {}, config);

        // wait until script is finished
        REQUIRE(exitFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
        CHECK(exitFuture.get() == true);
        CHECK(hitBp1 == 5);
        CHECK(hitBp2 == 25);
        CHECK(hitBp3 == 1);
    }

    TEST_CASE_FIXTURE(DebugFixture, "Debug_stopOnBreakpoint")
    {
        std::string fixturePath = getDebugFixturePath("simple.luau");
        Target target(*runtime);
        Breakpoint bp1 = target.setBreakpoint(fixturePath, 1);
        Breakpoint bp2 = target.setBreakpoint(fixturePath, 2);

        std::promise<void> hitPromise1;
        std::future<void> hitFuture1 = hitPromise1.get_future();
        std::promise<void> hitPromise2;
        std::future<void> hitFuture2 = hitPromise2.get_future();

        config.onBreakpointHit = [&](const Thread&, const Breakpoint& bp)
        {
            if (bp.id == bp1.id)
                hitPromise1.set_value();
            else if (bp.id == bp2.id)
                hitPromise2.set_value();
        };

        bool launched = target.launch(fixturePath, {}, config);
        CHECK(launched);
        // check we have hit the breakpoint 1
        REQUIRE(hitFuture1.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
        // check that we actually stopped at the breakpoint by having not hit the next breakpoint
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        REQUIRE(hitFuture2.wait_for(std::chrono::seconds(0)) == std::future_status::timeout);

        // continue execution
        bool continuedProcess = target.continueProcess();
        CHECK(continuedProcess);

        // check we have hit the breakpoint 2
        REQUIRE(hitFuture2.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
        // check that we actually stopped at the breakpoint by having not hit the exit
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        REQUIRE(exitFuture.wait_for(std::chrono::seconds(0)) == std::future_status::timeout);

        // continue execution
        continuedProcess = target.continueProcess();
        CHECK(continuedProcess);
        REQUIRE(exitFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    }

    TEST_CASE_FIXTURE(DebugFixture, "Debug_multiSource")
    {
        std::string mainPath = getDebugFixturePath("require_main.luau");
        std::string triangPath = getDebugFixturePath("require_triang.luau");

        int bpHit1 = 0, bpHit2 = 0, bpInstalled = 0;
        Target target(*runtime);
        Breakpoint bp1 = target.setBreakpoint(mainPath, 5);
        Breakpoint bp2 = target.setBreakpoint(triangPath, 5);
        config.onBreakpointInstall = [&](const Breakpoint& bp)
        {
            bpInstalled++;
        };
        config.onBreakpointHit = [&](const Thread&, const Breakpoint& bp)
        {
            if (bp.id == bp1.id)
                bpHit1++;
            else if (bp.id == bp2.id)
                bpHit2++;
            target.continueProcess();
        };
        bool launched = target.launch(mainPath, {}, config);
        CHECK(launched);
        // check we are done
        REQUIRE(exitFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
        CHECK(bpHit1 == 5);
        CHECK(bpHit2 == 15);
        CHECK(bpInstalled == 2);
        std::vector<std::string> sources = target.getLoadedSources();
        CHECK(sources.size() == 2);
        CHECK(std::find(sources.begin(), sources.end(), mainPath) != sources.end());
        CHECK(std::find(sources.begin(), sources.end(), triangPath) != sources.end());
    }
    TEST_CASE_FIXTURE(DebugFixture, "Debug_pauseProcess")
    {
        std::string fixturePath = getDebugFixturePath("loop.luau");
        Target target(*runtime);
        target.setBreakpoint(fixturePath, 1);

        int numPause = 0;
        std::promise<void> hitPromise;
        std::future<void> hitFuture = hitPromise.get_future();
        // This tests whether we pause after continuing. This is pretty
        // strange but is unfortunately, the best way of guaranteeing that a pause request
        // goes through without timing conerns.
        config.onBreakpointHit = [&](const Thread&, const Breakpoint& bp)
        {
            bool continuedProcess = target.continueProcess();
            CHECK(continuedProcess);
            bool pausedProcess = target.pauseProcess();
            CHECK(pausedProcess);
            hitPromise.set_value();
        };
        config.onPause = [&](const Thread& thread)
        {
            CHECK(thread.id == 0);
            numPause++;
        };
        bool launched = target.launch(fixturePath, {}, config);
        CHECK(launched);
        // we hit the breakpoint and should be paused now.
        REQUIRE(hitFuture.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
        std::this_thread::sleep_for(std::chrono::seconds(1));
        REQUIRE(exitFuture.wait_for(std::chrono::seconds(0)) == std::future_status::timeout);
        CHECK(numPause == 1);

        bool continuedProcess = target.continueProcess();
        CHECK(continuedProcess);
        REQUIRE(exitFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    }
    TEST_CASE_FIXTURE(DebugFixture, "Debug_destroyInfiniteLoop")
    {
        // This tests that we don't stall on the Runtime destruction even when we are running
        // an infinite loop.
        std::string fixturePath = getDebugFixturePath("infiniteloop.luau");

        std::future<void> destroyFuture = std::async(
            std::launch::async,
            [&]()
            {
                Target target(*runtime);
                target.launch(fixturePath, {}, config);
                // This is a timing pause to make sure that we actually start execution of the script.
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                // At this point target and its Runtime should be destroyed.
            }
        );
        REQUIRE(destroyFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    }
    TEST_CASE_FIXTURE(DebugFixture, "Debug_threadTracking")
    {
        std::string fixturePath = getDebugFixturePath("spawn.luau");
        Target target(*runtime);
        Breakpoint bp1 = target.setBreakpoint(fixturePath, 7);
        Breakpoint bp2 = target.setBreakpoint(fixturePath, 14);
        Breakpoint bp3 = target.setBreakpoint(fixturePath, 22);

        int maxThreadsSeen = 0;
        int hitsBp1 = 0, hitsBp2 = 0;
        config.onBreakpointHit = [&](const Thread& thread, const Breakpoint& bp)
        {
            if (bp.id == bp1.id)
            {
                CHECK(thread.id == 1);
                hitsBp1++;
                const std::vector<Thread>& threads = target.getThreads();
                maxThreadsSeen = std::max(maxThreadsSeen, (int)threads.size());
                if (threads.size() == 3)
                {
                    Thread t0(0, "Thread 0");
                    Thread t1(1, "Thread 1");
                    Thread t2(0, "Thread 2");
                    CHECK(std::find(threads.begin(), threads.end(), t0) != threads.end());
                    CHECK(std::find(threads.begin(), threads.end(), t1) != threads.end());
                    CHECK(std::find(threads.begin(), threads.end(), t2) != threads.end());
                }
            }
            else if (bp.id == bp2.id)
            {
                CHECK(thread.id == 2);
                hitsBp2++;
            }
            else
            {
                // after joining all threads, we only have one left over (the main coroutine)
                const std::vector<Thread>& threads = target.getThreads();
                CHECK(thread.id == 0);
                CHECK(threads.size() == 1);
                CHECK(threads.at(0) == Thread(0, "Thread 0"));
            }
            target.continueProcess();
            // we shouldn't get threads when things are crunning
        };
        target.launch(fixturePath, {}, config);
        REQUIRE(exitFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
        // over the course of execution, the maximum number of threads encountered should be 3
        CHECK(maxThreadsSeen == 3);
        CHECK(hitsBp1 == 5);
        CHECK(hitsBp2 == 5);
    }
    TEST_CASE_FIXTURE(DebugFixture, "Debug_stackFrame")
    {
        std::string fixturePath = getDebugFixturePath("recursion.luau");
        Target target(*runtime);
        Breakpoint bpA = target.setBreakpoint(fixturePath, 4);
        Breakpoint bpB = target.setBreakpoint(fixturePath, 8);
        Breakpoint mainBp = target.setBreakpoint(fixturePath, 14);
        int hits = 0;
        config.onBreakpointHit = [&](const Thread&, const Breakpoint& bp)
        {
            const std::vector<Thread>& threads = target.getThreads();
            CHECK(threads.size() == 1);
            int threadId = threads.at(0).id;
            std::optional<std::vector<StackFrame>> stacktrace = target.getStackTrace(threadId);
            REQUIRE(stacktrace.has_value());
            // our stack trace should from the most deep stack frame go main -> a -> b -> a -> b ....
            // the stack trace returns this in reverse since it starts from the shallowest stack frame.
            hits++;
            bool hitA = false;
            int expectedTraceSize = hits;
            if (bp.id == bpA.id)
            {
                hitA = true;
            }
            CHECK(stacktrace->size() == expectedTraceSize);

            for (int i = 0; i < expectedTraceSize; i++)
            {
                int line;
                int column = 0;
                std::string name;
                std::string sourcePath = fixturePath;
                if (i == expectedTraceSize - 1)
                {
                    line = 14;
                    name = "(anonymous)";
                }
                else if ((hitA && i % 2 == 0) || (!hitA && i % 2 == 1))
                {
                    line = 4;
                    name = "a";
                }
                else
                {
                    // the function name field is not set during the equality operation
                    // so B stays anonymous
                    name = "(anonymous)";
                    if (!hitA && i == 0)
                    {
                        line = 8;
                    }
                    else
                    {
                        line = 11;
                    }
                }
                StackFrame frame = stacktrace->at(i);
                CHECK(frame.id == i);
                CHECK(frame.column == column);
                CHECK(frame.line == line);
                CHECK(frame.name == name);
                CHECK(frame.sourcePath == sourcePath);
            }
            bool continued = target.continueProcess();
            CHECK(continued);
            stacktrace = target.getStackTrace(threadId);
            // we shouldn't get stack trace when things are paused
            REQUIRE(!stacktrace.has_value());
        };
        target.launch(fixturePath, {}, config);
        REQUIRE(exitFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
        CHECK(hits == 9);
    }

    TEST_CASE_FIXTURE(DebugFixture, "Debug_variables")
    {
        std::string fixturePath = getDebugFixturePath("variables.luau");
        Target target(*runtime);
        target.setBreakpoint(fixturePath, 10);

        std::vector<Variable> capturedLocals;
        config.onBreakpointHit = [&](const Thread&, const Breakpoint&)
        {
            const std::vector<Thread>& threads = target.getThreads();
            REQUIRE(threads.size() == 1);
            int threadId = threads.at(0).id;
            // stack frame at level 0
            std::optional<std::vector<VariableScope>> scopes = target.getScopes(threadId, 0);
            REQUIRE(scopes.has_value());
            checkScope(*scopes, VariableScopeType::Locals, "Locals", 0, 0);
            checkScope(*scopes, VariableScopeType::Upvalues, "Upvalues", 0, 0);
            std::optional<std::vector<Variable>> upvalues0 = target.getVariablesByContextType(threadId, 0, VariableScopeType::Upvalues);
            REQUIRE(upvalues0.has_value());
            checkVariable(*upvalues0, "a", "24", "number", false);
            checkVariable(*upvalues0, "_b", "1.2", "number", false);
            std::optional<std::vector<Variable>> locals0 = target.getVariablesByContextType(threadId, 0, VariableScopeType::Locals);
            checkVariable(*locals0, "_k", "25.2", "number", false);
            // stack frame at level 1
            scopes = target.getScopes(threadId, 1);
            REQUIRE(scopes.has_value());
            checkScope(*scopes, VariableScopeType::Locals, "Locals", 0, 1);
            checkScope(*scopes, VariableScopeType::Upvalues, "Upvalues", 0, 1);
            std::optional<std::vector<Variable>> locals1 = target.getVariablesByContextType(threadId, 1, VariableScopeType::Locals);
            REQUIRE(locals1.has_value());
            checkVariable(*locals1, "a", "24", "number", false);
            checkVariable(*locals1, "_b", "1.2", "number", false);
            checkVariable(*locals1, "_c", "\"test\"", "string", false);
            int ref1 = checkVariable(*locals1, "_d", "{1, 2, 3}", "table", true);
            int ref2 = checkVariable(*locals1, "_e", "{a=4, [4]={r=13}, b=\"5\"}", "table", true);
            checkVariable(*locals1, "_f", "true", "boolean", false);
            std::optional<std::vector<Variable>> table = target.getVariables(ref1);
            REQUIRE(table.has_value());
            checkVariable(*table, "[1]", "1", "number", false);
            checkVariable(*table, "[2]", "2", "number", false);
            checkVariable(*table, "[3]", "3", "number", false);
            table = target.getVariables(ref2);
            REQUIRE(table.has_value());
            checkVariable(*table, "a", "4", "number", false);
            int ref3 = checkVariable(*table, "[4]", "{r=13}", "table", true);
            checkVariable(*table, "b", "\"5\"", "string", false);
            table = target.getVariables(ref3);
            REQUIRE(table.has_value());
            checkVariable(*table, "r", "13", "number", false);
            std::optional<std::vector<Variable>> upvalues1 = target.getVariablesByContextType(threadId, 1, VariableScopeType::Upvalues);
            REQUIRE(upvalues1.has_value());
            CHECK(upvalues1->size() == 0);
            target.continueProcess();
        };
        target.launch(fixturePath, {}, config);
        REQUIRE(exitFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    };
}
