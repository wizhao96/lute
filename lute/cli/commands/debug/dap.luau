local debugger = require("@lute/debugger")
local dapStream = require("./dapStream")
local dapTypes = require("./dapTypes")
local json = require("@std/json")

local function debugToDapBreakpoint(breakpoint: debugger.Breakpoint): dapTypes.Breakpoint
	local dapBreakpoint: dapTypes.Breakpoint = {
		id = breakpoint.id,
		verified = breakpoint.status == "installed",
		source = {
			path = breakpoint.sourcePath,
		},
		line = breakpoint.line,
	}
	if breakpoint.status == "pendingInstall" then
		dapBreakpoint.reason = "pending"
	end
	if breakpoint.status == "invalid" then
		dapBreakpoint.reason = "failed"
	end
	return dapBreakpoint
end

local function clearAndSetBreakpoints(
	target: debugger.Target,
	sourcePath: string,
	newBreakpoints: { dapTypes.SourceBreakpoint }?
): { dapTypes.Breakpoint }
	local currentBps = target:getBreakpoints()
	for _, bp in currentBps do
		if bp.sourcePath == sourcePath then
			target:removeBreakpoint(bp.id)
		end
	end
	local requestedBps = {}
	if newBreakpoints then
		for _, setBp in newBreakpoints do
			table.insert(requestedBps, debugToDapBreakpoint(target:setBreakpoint(sourcePath, setBp.line)))
		end
	end
	return requestedBps
end

local function dapLoop(reader: dapStream.Reader, writer: dapStream.Writer)
	local ioSession = dapStream.makeDapStreamSession(reader, writer)
	local target = debugger.newTarget()
	local launchCallbacks: debugger.LaunchConfig = {
		onBreakpointHit = function(thread: debugger.Thread, bp: debugger.Breakpoint)
			ioSession.writeEvent("stopped", {
				reason = "breakpoint",
				threadId = thread.id,
				hitBreakpointIds = { bp.id },
				allThreadsStopped = true,
			})
		end,
		onPause = function()
			ioSession.writeEvent("stopped", {
				reason = "pause",
				allThreadsStopped = true,
			})
		end,
		onBreakpointInstall = function(bp: debugger.Breakpoint)
			ioSession.writeEvent("breakpoint", {
				reason = "changed",
				breakpoint = debugToDapBreakpoint(bp),
			})
		end,
		onExit = function(success: boolean)
			local exitCode = if success then 0 else 1
			ioSession.writeEvent("exited", {
				exitCode = exitCode,
			})
			ioSession.writeEvent("terminated")
		end,
	}
	local configurationDone = false
	local launchConfig: dapTypes.LaunchArgs? = nil
	local launchSeq = -1
	while true do
		local request = ioSession.readDap()
		local command = request.command :: string
		local reqSeq = request.seq :: number
		if command == "initialize" then
			local capabilities = {
				supportsConfigurationDoneRequest = true,
				supportsTerminateRequest = true,
			}
			ioSession.writeResponse(reqSeq, true, command, nil, capabilities)
			ioSession.writeEvent("initialized")
		elseif command == "configurationDone" then
			configurationDone = true
			-- clients can send either configurationDone before or after launch.
			-- in the second case, we delay sending a response to the launch request until after
			-- configuration is done.
			ioSession.writeResponse(reqSeq, true, command)
			if launchConfig then
				target:launch(launchConfig.program, launchConfig.args, launchCallbacks)
				ioSession.writeResponse(launchSeq, true, "launch")
			end
		elseif command == "setBreakpoints" then
			local args = request.arguments :: json.Object
			local source = args.source :: dapTypes.Source
			local path = source.path :: string
			local newBreakpoints = args.breakpoints :: { dapTypes.SourceBreakpoint }?
			local requestedBreakpoints = clearAndSetBreakpoints(target, path, newBreakpoints)

			ioSession.writeResponse(reqSeq, true, command, nil, {
				breakpoints = requestedBreakpoints,
			})
		elseif command == "continue" then
			target:continueProcess()
			ioSession.writeResponse(reqSeq, true, command)
		elseif command == "pause" then
			target:pauseProcess()
			ioSession.writeResponse(reqSeq, true, command)
		elseif command == "launch" then
			local args = request.arguments :: dapTypes.LaunchArgs?
			if args and args.program then
				launchConfig = {
					program = args.program,
					args = args.args,
				}
				launchSeq = reqSeq
				-- configuration is already done, respond to launch
				if configurationDone then
					target:launch(launchConfig.program, launchConfig.args, launchCallbacks)
					ioSession.writeResponse(reqSeq, true, command)
				end
			end
		elseif command == "loadedSources" then
			local sources = target:getLoadedSources()
			local dapSources = {}
			for _, source in sources do
				table.insert(dapSources, {
					path = source,
					name = source:match("[^/\\]+$"),
				})
			end
			ioSession.writeResponse(reqSeq, true, command, nil, {
				sources = dapSources,
			})
		-- dummy handlers for threads and stackTrace for now
		elseif command == "threads" then
			local threads = target:getThreads()
			if not threads then
				ioSession.writeResponse(reqSeq, false, command, "notStopped")
				continue
			end
			assert(threads)
			local dapThreads = {}
			for _, thread in threads do
				table.insert(dapThreads, {
					id = thread.id,
					name = thread.name,
				})
			end
			ioSession.writeResponse(reqSeq, true, command, nil, {
				threads = dapThreads,
			})
		elseif command == "stackTrace" then
			local args = request.arguments :: json.Object
			local threadId = args.threadId :: number
			local startFrame = args.startFrame :: number?
			local levels = args.levels :: number?
			local stackTrace = target:getStackTrace(threadId, startFrame, levels)
			if not stackTrace then
				ioSession.writeResponse(reqSeq, false, command, "notStopped")
				continue
			end
			assert(stackTrace)
			local dapStackTrace = {}
			for _, frame in stackTrace do
				table.insert(dapStackTrace, {
					id = frame.id,
					name = frame.name,
					source = {
						path = frame.sourcePath,
					},
					line = frame.line,
					column = frame.column,
				})
			end
			ioSession.writeResponse(reqSeq, true, command, nil, {
				stackFrames = dapStackTrace,
			})
		elseif command == "terminate" or command == "disconnect" then
			ioSession.writeResponse(reqSeq, true, command)
			ioSession.writeEvent("terminated")
			break
		else
			ioSession.writeResponse(reqSeq, false, command, `unsupported command: {command}`)
		end
	end
end

return {
	dapLoop = dapLoop,
}
