local test = require("@std/test")

local task = require("@lute/task")
local debug = require("@lute/debugger")
local path = require("@std/path")
local proc = require("@std/process")

-- a helper function that basically polls a boolean, since Luau does
-- not allow the promises/futures we use in the C++ tests
local function waitUntil(condition: () -> boolean, timeout: number?, pollInterval: number?): boolean
	local elapsed = 0
	local limit = timeout or 5
	local poll = pollInterval or 0.05
	while not condition() and elapsed < limit do
		task.wait(poll)
		elapsed += poll
	end
	return condition()
end

local function checkScope(
	check: any,
	scopes: { debug.VariableScope },
	name: string,
	scopeType: debugger.VariableScopeType,
	threadId: number,
	level: number
)
	local found = nil
	for _, scope in scopes do
		if scope.name == name then
			found = scope
			break
		end
	end
	check.that(found ~= nil)
	if found then
		check.eq(found.type, scopeType)
		check.eq(found.threadId, threadId)
		check.eq(found.level, level)
		check.that(found.variableRef > 0)
	end
end

local function checkVariable(
	check: any,
	vars: { debug.Variable },
	name: string,
	value: string,
	varType: string,
	isTable: boolean
): number
	local found = nil
	for _, var in vars do
		if var.name == name then
			found = var
			break
		end
	end
	check.that(found ~= nil)
	if found then
		check.eq(found.value, value)
		check.eq(found.type, varType)
		if isTable then
			check.that(found.variableRef > 0)
		else
			check.eq(found.variableRef, 0)
		end
		return found.variableRef
	end
	return 0
end

test.suite("LuteDebugLibrarySuite", function(suite)
	-- this test cause mainly checks launching and hitting breakpoints
	suite:case("launchAndHitBreakpoint", function(check)
		local target = debug.newTarget()
		check.eq(typeof(target), "Target")
		local fixturePath = path.format(path.join(proc.cwd(), "tests/src/debug/loop.luau"))
		local bp = target:setBreakpoint(fixturePath, 6)
		check.eq(bp.id, 0)
		check.eq(bp.line, 6)
		check.eq(bp.sourcePath, fixturePath)
		check.eq(bp.status, "pendingInstall")
		local numHits, onFinished, numBreakpointsInstalled = 0, false, 0
		local targetLaunched = target:launch(fixturePath, {}, {
			onBreakpointHit = function(_thread, _bp)
				numHits += 1
				local continued = target:continueProcess()
				check.that(continued)
			end,
			onBreakpointInstall = function(_bp)
				numBreakpointsInstalled += 1
			end,
			onExit = function(status)
				if status then
					onFinished = true
				end
			end,
		})
		check.that(targetLaunched)
		check.eq(
			waitUntil(function()
				return onFinished
			end),
			true
		)
		check.eq(numHits, 25)
		check.eq(numBreakpointsInstalled, 1)
	end)
	-- this test case focuses on adding, getting, and removing breakpoints
	suite:case("setAndRemove", function(check)
		local target = debug.newTarget()
		check.eq(typeof(target), "Target")
		local fixturePath = path.format(path.join(proc.cwd(), "tests/src/debug/loop.luau"))
		local bp1 = target:setBreakpoint(fixturePath, 6)
		local bp2 = target:setBreakpoint(fixturePath, 7)
		local bp3 = target:setBreakpoint(fixturePath, 4)
		local bpGet = assert(target:getBreakpointById(bp1.id))
		check.eq(bpGet.id, bp1.id)
		check.eq(bpGet.line, 6)
		check.eq(bpGet.sourcePath, fixturePath)
		check.eq(bpGet.status, "pendingInstall")
		bpGet = assert(target:getBreakpointById(bp2.id))
		check.eq(bpGet.id, bp2.id)
		check.eq(bpGet.line, 7)
		check.eq(bpGet.sourcePath, fixturePath)
		check.eq(bpGet.status, "pendingInstall")
		bpGet = assert(target:getBreakpointById(bp3.id))
		check.eq(bpGet.id, bp3.id)
		check.eq(bpGet.line, 4)
		check.eq(bpGet.sourcePath, fixturePath)
		check.eq(bpGet.status, "pendingInstall")
		local bps = target:getBreakpoints()
		check.eq(#bps, 3)
		target:removeBreakpoint(bp1.id)
		bps = target:getBreakpoints()
		check.eq(#bps, 2)
		bpGet = target:getBreakpointById(bp1.id)
		check.eq(bpGet, nil)
		local numHitsBp2, numHitsBp3, onFinished, numBreakpointsUninstalled = 0, 0, false, 0
		local targetLaunched = target:launch(fixturePath, {}, {
			onBreakpointHit = function(_thread, bp)
				if bp.id == bp2.id then
					check.eq(bp.id, bp2.id)
					check.eq(bp.line, 7)
					check.eq(bp.sourcePath, fixturePath)
					check.eq(bp.status, "installed")
					numHitsBp2 += 1
					target:removeBreakpoint(bp.id)
					-- the removal should happen immediately given we are in paused state
					bps = target:getBreakpoints()
					check.eq(#bps, 1)
				else
					numHitsBp3 += 1
				end
				local continued = target:continueProcess()
				check.that(continued)
			end,
			onBreakpointUninstall = function(_bp)
				numBreakpointsUninstalled += 1
			end,
			onExit = function(status)
				if status then
					onFinished = true
				end
			end,
		})
		check.that(targetLaunched)
		-- we expect this breakpoint to be immediately installed
		bpGet = assert(target:getBreakpointById(bp3.id))
		check.eq(bpGet.id, bp3.id)
		check.eq(bpGet.line, 4)
		check.eq(bpGet.sourcePath, fixturePath)
		check.eq(bpGet.status, "installed")

		check.eq(
			waitUntil(function()
				return onFinished
			end),
			true
		)
		bps = target:getBreakpoints()
		check.eq(#bps, 1)
		check.eq(numHitsBp2, 1)
		check.eq(numHitsBp3, 5)
		check.eq(numBreakpointsUninstalled, 1)
	end)
	-- this test case checks bp actually pauses execution
	suite:case("pauseBp", function(check)
		local target = debug.newTarget()
		check.eq(typeof(target), "Target")
		local fixturePath = path.format(path.join(proc.cwd(), "tests/src/debug/simple.luau"))
		local onFinished = false
		target:setBreakpoint(fixturePath, 2)
		local targetLaunched = target:launch(fixturePath, {}, {
			onExit = function(status)
				if status then
					onFinished = true
				end
			end,
		})
		check.that(targetLaunched)
		check.eq(
			waitUntil(function()
				return onFinished
			end, 1),
			false
		)
		local continued = target:continueProcess()
		check.that(continued)
		check.eq(
			waitUntil(function()
				return onFinished
			end, 1),
			true
		)
	end)
	-- this test case focuses on multiple sources
	suite:case("multisource", function(check)
		local target = debug.newTarget()
		check.eq(typeof(target), "Target")
		local mainPath = path.format(path.join(proc.cwd(), "tests/src/debug/require_main.luau"))
		local triangPath = path.format(path.join(proc.cwd(), "tests/src/debug/require_triang.luau"))
		local bp1 = target:setBreakpoint(mainPath, 5)
		local _bp2 = target:setBreakpoint(triangPath, 5)
		local bpHit1 = 0
		local bpHit2 = 0
		local onFinished = false
		local targetLaunched = target:launch(mainPath, {}, {
			onBreakpointHit = function(_thread, bp)
				if bp.id == bp1.id then
					bpHit1 += 1
				else
					bpHit2 += 1
				end
				local continued = target:continueProcess()
				check.that(continued)
			end,
			onExit = function(status)
				if status then
					onFinished = true
				end
			end,
		})
		check.that(targetLaunched)
		check.eq(
			waitUntil(function()
				return onFinished
			end),
			true
		)
		check.eq(bpHit1, 5)
		check.eq(bpHit2, 15)
		local source = target:getLoadedSources()
		check.that(table.find(source, mainPath))
		check.that(table.find(source, triangPath))
	end)
	-- this test case focuses on multiple threads
	suite:case("multithread", function(check)
		local target = debug.newTarget()
		check.eq(typeof(target), "Target")
		local mainPath = path.format(path.join(proc.cwd(), "tests/src/debug/spawn.luau"))
		local bp1 = target:setBreakpoint(mainPath, 7)
		local _bp2 = target:setBreakpoint(mainPath, 14)
		local bpHit1 = 0
		local bpHit2 = 0
		local onFinished = false
		local maxThreads = 0
		local targetLaunched = target:launch(mainPath, {}, {
			onBreakpointHit = function(thread, bp)
				if bp.id == bp1.id then
					check.eq(thread.id, 1)
					bpHit1 += 1
				else
					check.eq(thread.id, 2)
					bpHit2 += 1
				end
				local threads = target:getThreads()
				maxThreads = math.max(maxThreads, #threads)
				local continued = target:continueProcess()
				check.that(continued)
			end,
			onExit = function(status)
				if status then
					onFinished = true
				end
			end,
		})
		check.that(targetLaunched)
		check.eq(
			waitUntil(function()
				return onFinished
			end),
			true
		)
		check.eq(bpHit1, 5)
		check.eq(bpHit2, 5)
		check.eq(maxThreads, 3)
	end)
	-- this test case focuses on inspecting stack trace
	suite:case("inspectStackTrace", function(check)
		local target = debug.newTarget()
		check.eq(typeof(target), "Target")
		local mainPath = path.format(path.join(proc.cwd(), "tests/src/debug/pow.luau"))
		target:setBreakpoint(mainPath, 5)
		local hits = 0
		local onFinished = false
		local targetLaunched = target:launch(mainPath, {}, {
			onBreakpointHit = function(thread, bp)
				hits += 1
				local threads = target:getThreads()
				check.eq(#threads, 1)
				local stacktrace = target:getStackTrace(threads[1].id)
				assert(stacktrace)
				check.eq(#stacktrace, hits + 1)
				for i, frame in stacktrace do
					check.eq(frame.id, i - 1)
					check.eq(frame.column, 0)
					check.eq(frame.sourcePath, mainPath)
					if i == #stacktrace then
						check.eq(frame.name, "(anonymous)")
						check.eq(frame.line, 8)
					else
						check.eq(frame.name, "pow")
						check.eq(frame.line, 5)
					end
				end
				local continued = target:continueProcess()
				check.that(continued)
			end,
			onExit = function(status)
				if status then
					onFinished = true
				end
			end,
		})
		check.that(targetLaunched)
		check.eq(
			waitUntil(function()
				return onFinished
			end),
			true
		)
	end)
	-- this test case focuses on inspecting variables
	suite:case("inspectStackTrace", function(check)
		local target = debug.newTarget()
		check.eq(typeof(target), "Target")
		local mainPath = path.format(path.join(proc.cwd(), "tests/src/debug/variables.luau"))
		target:setBreakpoint(mainPath, 10)
		local onFinished = false
		local targetLaunched = target:launch(mainPath, {}, {
			onBreakpointHit = function(thread, bp)
				local threads = target:getThreads()
				check.eq(#threads, 1)
				local stacktrace = target:getStackTrace(threads[1].id)
				assert(stacktrace)
				check.eq(#stacktrace, 2)
				local scopes = target:getScopes(stacktrace[1].id)
				assert(scopes)
				check.eq(#scopes, 2)
				checkScope(check, scopes, "Locals", "locals", threads[1].id, stacktrace[1].id)
				checkScope(check, scopes, "Upvalues", "upvalues", threads[1].id, stacktrace[1].id)
				for _, scope in scopes do
					if scope.type == "locals" then
						local locals = target:getVariables(scope.variableRef)
						assert(locals)
						check.eq(#locals, 1)
						checkVariable(check, locals, "_k", "25.2", "number", false)
					end
					if scope.type == "upvalues" then
						local upvalues = target:getVariables(scope.variableRef)
						assert(upvalues)
						check.eq(#upvalues, 2)
						checkVariable(check, upvalues, "a", "24", "number", false)
						checkVariable(check, upvalues, "_b", "1.2", "number", false)
					end
				end
				local locals = target:getVariablesByScopeType(stacktrace[2].id, "locals")
				assert(locals)
				check.eq(#locals, 7)
				checkVariable(check, locals, "a", "24", "number", false)
				checkVariable(check, locals, "_b", "1.2", "number", false)
				checkVariable(check, locals, "_c", '"test"', "string", false)
				local ref1 = checkVariable(check, locals, "_d", "{1, 2, 3}", "table", true)
				checkVariable(check, locals, "_e", '{a=4, [4]={r=13}, b="5"}', "table", true)
				checkVariable(check, locals, "_f", "true", "boolean", false)
				checkVariable(check, locals, "g", "function", "function", false)
				local tables = target:getVariables(ref1)
				assert(tables)
				check.eq(#tables, 3)
				checkVariable(check, tables, "[1]", "1", "number", false)
				checkVariable(check, tables, "[2]", "2", "number", false)
				checkVariable(check, tables, "[3]", "3", "number", false)
				local continued = target:continueProcess()
				check.that(continued)
			end,
			onExit = function(status)
				if status then
					onFinished = true
				end
			end,
		})
		check.that(targetLaunched)
		check.eq(
			waitUntil(function()
				return onFinished
			end),
			true
		)
	end)
	-- this test case focuses on stepping
	suite:case("multisource", function(check)
		local target = debug.newTarget()
		check.eq(typeof(target), "Target")
		local mainPath = path.format(path.join(proc.cwd(), "tests/src/debug/step.luau"))
		target:setBreakpoint(mainPath, 9)
		local bpHit = false
		local stepsDone = 0
		local onFinished = false
		local targetLaunched = target:launch(mainPath, {}, {
			onStepStop = function(info)
				stepsDone += 1
			end,
			onBreakpointHit = function(bp)
				bpHit = true
			end,
			onExit = function(status)
				if status then
					onFinished = true
				end
			end,
		})
		check.that(targetLaunched)
		check.eq(
			waitUntil(function()
				return bpHit
			end),
			true
		)
		check.eq(target:getLine(), 9)
		target:step("stepIn")
		check.eq(
			waitUntil(function()
				return stepsDone == 1
			end),
			true
		)
		check.eq(target:getLine(), 10)
		target:stepIn()
		check.eq(
			waitUntil(function()
				return stepsDone == 2
			end),
			true
		)
		check.eq(target:getLine(), 2)
		target:stepOut()
		check.eq(
			waitUntil(function()
				return stepsDone == 3
			end),
			true
		)
		check.eq(target:getLine(), 11)
		target:stepOver()
		check.eq(
			waitUntil(function()
				return stepsDone == 4
			end),
			true
		)
		check.eq(target:getLine(), 12)
		target:continueProcess()
		check.eq(
			waitUntil(function()
				return onFinished
			end),
			true
		)
	end)
end)
