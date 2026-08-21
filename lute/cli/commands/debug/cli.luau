local debugger = require("@lute/debugger")
local io = require("@std/io")
local path = require("@std/path")
local proc = require("@std/process")
local fs = require("@std/fs")
local task = require("@std/task")

local function resolvePath(userPath: string): string
	if path.isAbsolute(userPath) then
		return path.format(userPath)
	end
	return path.format(path.join(proc.cwd(), userPath))
end

local function parseLine(line: string): (string, string)
	local trimmed = line:match("^%s*(.-)%s*$") or ""
	local command, rest = trimmed:match("^(%S+)%s*(.*)$")
	return command or "", rest or ""
end

local sourceLineCache: { [string]: { string } } = {}

local function getSourceLines(sourcePath: string): { string }?
	if sourceLineCache[sourcePath] then
		return sourceLineCache[sourcePath]
	end
	local ok, handle = pcall(fs.open, sourcePath, "r")
	if not ok then
		return nil
	end
	local readOk, contents = pcall(fs.read, handle)
	fs.close(handle)
	if not readOk then
		return nil
	end
	local lines = {}
	for line in (contents .. "\n"):gmatch("(.-)\n") do
		table.insert(lines, line)
	end
	sourceLineCache[sourcePath] = lines
	return lines
end

local function printSourceContext(sourcePath: string, line: number, context: number?)
	local ctx = context or 2
	local lines = getSourceLines(sourcePath)
	if not lines then
		io.write(`(could not read source: {sourcePath})\n`)
		return
	end
	local startLine = math.max(1, line - ctx)
	local endLine = math.min(#lines, line + ctx)
	for i = startLine, endLine do
		local marker = if i == line then "->" else "  "
		io.write(`{marker} {i}\t{lines[i]}\n`)
	end
end

local function cli()
	local runningCLI = true
	local target = debugger.newTarget()
	local currentThreadId: number? = nil
	local currentFrameId: number? = nil
	local lastInput = ""
	local stopped = true
	local launched = false

	local function onBreakpointHit(thread, bp)
		currentThreadId = thread.id
		local stackTrace = target:getStackTrace(thread.id)
		currentFrameId = stackTrace and stackTrace[1] and stackTrace[1].id
		io.write(`Breakpoint {bp.id}, stopped at {bp.sourcePath}:{bp.line}\n`)
		printSourceContext(bp.sourcePath, bp.line)
		stopped = true
	end
	local function onExit(success: boolean)
		local savedBps = target:getBreakpoints()
		if success then
			io.write("Program exited normally.\n")
		else
			io.write("Program exited with an error.\n")
		end
		currentThreadId = nil
		currentFrameId = nil
		target = debugger.newTarget()
		for savedBp in savedBps do
			target.setBreakpoint(savedBp.source, savedBp.line)
		end
		stopped = true
		launched = false
	end
	local function printVariable(v: debugger.Variable, useEval: boolean)
		if not useEval then
			io.write(`name: {v.name}\n`)
		end
		io.write(`value: {v.value}\n`)
		io.write(`type: {v.type}\n`)
		if v.variableRef and v.variableRef > 0 then
			io.write(`subvalues:\n`)
			local members = target:getVariables(v.variableRef)
			if members then
				for _, m in members do
					if m.variableRef and m.variableRef > 0 then
						io.write(`\t{m.name} = {m.value} (var ref {m.variableRef})\n`)
					else
						io.write(`\t{m.name} = {m.value}\n`)
					end
				end
			end
		end
	end
	while runningCLI do
		while not stopped do
			task.wait()
		end
		local input = io.input("(lute debug)\t")
		if input == "" and lastInput ~= "" then
			input = lastInput
		else
			lastInput = input
		end
		local command, rest = parseLine(input)
		if command == "b" or command == "bp" or command == "breakpoint" then
			local bpCommand, restBp = parseLine(rest)
			if bpCommand == "set" or bpCommand == "add" then
				local sourcePath, line = restBp:match("^(.-):(%d+)$")
				if sourcePath and line and tonumber(line) then
					local bp = target:setBreakpoint(resolvePath(sourcePath), tonumber(line))
					io.write(`Breakpoint {bp.id} now set at {bp.sourcePath}:{bp.line}\n`)
				else
					io.write(string.format("usage: %s %s <file>:<line>\n", command, bpCommand))
				end
			elseif bpCommand == "r" or bpCommand == "remove" then
				local id = tonumber(restBp)
				if id then
					target:removeBreakpoint(id)
				else
					io.write("usage: breakpoint remove <id>\n")
				end
			elseif bpCommand == "l" or bpCommand == "list" then
				local bps = target:getBreakpoints()
				for _, bp in bps do
					io.write(`Breakpoint {bp.id}: {bp.sourcePath}:{bp.line}\n`)
				end
			end
		elseif command == "launch" or command == "run" or command == "r" then
			local sourcePath, argsRest = parseLine(rest)
			if sourcePath == "" then
				io.write(string.format("usage: %s <script> [args...]\n", command))
			elseif launched then
				io.write("error: cannot relaunch running target\n")
			else
				local args = {}
				for arg in argsRest:gmatch("%S+") do
					table.insert(args, arg)
				end
				local err = target:launch(resolvePath(sourcePath), args, {
					onBreakpointHit = onBreakpointHit,
					onExit = onExit,
				})
				if err then
					io.write(`error: {err}\n`)
				else
					stopped = false
					launched = true
				end
			end
		elseif command == "p" or command == "print" or command == "expr" then
			if not currentFrameId then
				io.write("no frame selected (not stopped/launched)\n")
			else
				local result = target:evaluateExpression(rest, currentFrameId)
				if type(result) == "string" then
					io.write(`error: {result}\n`)
				else
					printVariable(result, true)
				end
			end
		elseif command == "p" or command == "print" or command == "expr" then
			if not currentFrameId then
				io.write("no frame selected (not stopped/launched)\n")
			else
				local result = target:evaluateExpression(rest, currentFrameId)
				if type(result) == "string" then
					io.write(`error: {result}\n`)
				else
					printVariable(result, true)
				end
			end
		elseif command == "continue" or command == "c" then
			local continued = target:continueProcess()
			if not continued then
				io.write("cannot continue (not stopped/launched)\n")
			else
				stopped = false
			end
		elseif command == "q" or command == "quit" then
			runningCLI = false
		else
			io.write(`unknown command: \"{command}\"\n`)
		end
	end
end

return {
	cli = cli,
}
