-- lute-lint-global-ignore(unused_variable)
local debugger = {}

--- The status of a breakpoint, where pendingInstall and pendingUninstall means that the corresponding
--- operation is queued to happen when the script is next paused.
export type BreakpointStatus = "pendingInstall" | "pendingUninstall" | "installed" | "invalid"


--- additional options for a breakpoint
export type BreakpointConfig = {
	condition: string?,
	hitCondition: string?,
	logMessage: string?,
}

--- A breakpoint that stops execution when encountered during runtime
--- if condition is not "", the condition must evaluate to true before stopping.
--- if hitCondition is not "", a condition involving the hitCount must be true before stopping.
--- if logMessage is not "", then instead of stopping we simply output an interpolated log message to the relevant callback.
export type Breakpoint = {
	id: number,
	line: number,
	sourcePath: string,
	condition: string,
	hitCondition: string,
	hitCount: number,
	logMessage: string,
	status: BreakpointStatus,
}

--- The type of step we are taking
export type StepType = "stepIn" | "stepOut" | "stepOver"

--- Information about the current step() command we are running
export type StepInfo = {
	type: StepType,
	startLine: number,
	startDepth: number,
}

--- A thread of execution, which represents one coroutine running on the runtime
export type Thread = {
	id: number,
	name: string,
}

--- A stack frame of the execution. New stack frames are recreated every time execution is paused, and
--- ids are reused.
export type StackFrame = {
	id: number,
	name: string,
	sourcePath: string,
	line: number,
	column: number,
}

--- The type of a VariableScope (i.e. whether it contains local variables, upvalues, or
--- values from a table)
export type VariableScopeType = "locals" | "upvalues" | "table"

--- A VariableScope contains the contextual information for a collection of Variables
--- such as what variable reference ID number to refer to this collection, what type of collection
--- it is, etc. Variable reference IDs are reset upon continuing execution.
export type VariableScope = {
	variableRef: number,
	type: VariableScopeType,
	name: string,
	threadId: number,
	level: number,
}

--- A Variable represents an inspected variable from the script, including its name,
--- type, and a value.
--- If the Variable represented is a table, its value may be condensed
--- to a shorter one-line summary. It will then have a variableRef > 0 that can be inspected further.
export type Variable = {
	name: string,
	value: string,
	type: string,
	variableRef: number,
}

--- A set of callbacks that we configure before launching our debugger
export type LaunchConfig = {
	--- called when a breakpoint `bp` is hit on thread `thread`; execution is paused until continueProcess() is called
	onBreakpointHit: ((thread: Thread, bp: Breakpoint) -> ())?,
	--- called when breakpoint `bp` is attempted to be installed in the VM.
	--- whether this attempt is successful or not is reflected in the status of bp.
	onBreakpointInstall: ((bp: Breakpoint) -> ())?,
	--- called when a breakpoint `bp` is uninstalled from the VM
	onBreakpointUninstall: ((bp: Breakpoint) -> ())?,
	--- called when the debugged script exits. `success` reflects if the script exited without errors.
	onExit: ((success: boolean) -> ())?,
	--- called when a pauseProcess() request actually stops all threads from execution. this pause
	--- interrupts `thread`.
	onPause: ((thread: Thread) -> ())?,
	--- called when our script finishes stepping on `thread`. `info` reflects the information about our stepping command.
	onStepStop: ((thread: Thread, info: StepInfo) -> ())?,
	--- called when print() would have printed out `message` and is called from `source` on line number `line`
	onPrint: ((message: string, source: string, line: number) -> ())?,
	--- called when we hit a log point at `source` on line number `line`
	onLogpointHit: ((message: string, bp: Breakpoint) -> ())?,
}

--- A `Target` represents the target program under the debugger.
export type Target = {
	--- Sets a breakpoint at `sourcePath` with a given line and queue it for installation.
	--- If this bp already existed return the preexisting breakpoint. Otherwise, it returns the new one.
	setBreakpoint: (self: Target, sourcePath: string, line: number, config: BreakpointConfig?) -> Breakpoint,
	--- Remove a breakpoint with a given `id` and queue for uninstall.
	--- Returns whether it was successful or not.
	removeBreakpoint: (self: Target, id: number) -> boolean,
	--- Get all breakpoints belonging to a target.
	getBreakpoints: (self: Target) -> { Breakpoint },
	--- Get all breakpoints belonging to a target with a certain status.
	getBreakpointsByStatus: (self: Target, status: BreakpointStatus) -> { Breakpoint },
	--- Get a breakpoint with a given id. Returns `nil` if not found.
	getBreakpointById: (self: Target, id: number) -> Breakpoint?,
	--- Get a breakpoint at a `sourcePath` at a given line. Returns `nil` if not found.
	getBreakpointBySourceLine: (self: Target, sourcePath: string, line: number) -> Breakpoint?,
	--- Launch a Luau script at at `sourcePath` with the relevant `args` and callbacks.
	--- Returns whether the script could be launched.
	launch: (self: Target, sourcePath: string, args: { string }?, config: LaunchConfig?) -> boolean,
	--- Continues a target script if it has been stopped. Return `true` if the target was actually stopped
	--- and `false` if it was not.
	continueProcess: (self: Target) -> boolean,
	--- Pauses a target script if it is running at the earliest possible opportunity. Return `true`
	--- if the target was actually running and `false` if it was not.
	pauseProcess: (self: Target) -> boolean,
	--- Gets a list of all Luau coroutines that are being used by the script.
	getThreads: (self: Target) -> { Thread },
	--- Gets our current line of execution when we are stopped.
	--- Returns our current line or -1 if we are not stopped.
	getLine: (self: Target) -> number,
	--- Gets a stack frame on a coroutine with `threadId` at level `level` (where 0 is the shallowest). Return `nil`
	--- if script is running or no such stack frame exists at that level.
	getStackFrame: (self: Target, threadId: number, level: number) -> StackFrame?,
	--- Gets a stack trace on a coroutine with `threadId` starting from level `startLevel` and going for at maximum
	--- `numFrames` frames. If `startLevel` is not specified, we start at level 0. If `numFrames` is not specified,
	--- we return until the deepest frame.
	getStackTrace: (self: Target, threadId: number, startLevel: number?, numFrames: number?) -> { StackFrame }?,
	--- Gets a set of variable scopes on the stack frame with id `frameId`. Return `nil` if script is running
	--- or if that stack frame is not found.
	getScopes: (self: Target, frameId: number) -> { VariableScope }?,
	--- Gets a set of variables belonging to the scope refered by `variableRef`. Return `nil` if script is running
	--- or if such a scope does not exist.
	getVariables: (self: Target, variableRef: number) -> { Variable }?,
	--- Gets a set of variables belonging on the stack frame with id `frameID` with a certain scopeType.
	--- Return `nil` if script is running or if such variables can not be found.
	getVariablesByScopeType: (self: Target, frameId: number, sourceType: VariableScopeType) -> { Variable }?,
	--- Evaluates an expression within the scope of a given stack frame id. 
	--- If `frameId` is -1, evaluate in the global scope.
	--- Returns a `Variable` if expression could be evaluated or an error string if not.
	evaluateExpression: (self: Target, expression: string, frameId: number) -> Variable | string,
	--- Steps into the next line of the code on `threadId`, including into a function if the line contains a function.
	--- Returns `true` if target was paused before stepping and `false` otherwise.
	step: (self: Target, threadId: number, type: StepType) -> boolean,
	--- Steps into the next line of the code on `threadId`, including into a function if the line contains a function.
	--- Returns `true` if target was paused before stepping and `false` otherwise.
	stepIn: (self: Target, threadId: number) -> boolean,
	--- Steps over to the next line of the code on `threadId`, skipping over any function calls if our current line contains a function.
	--- Returns `true` if target was paused before stepping and `false` otherwise.
	stepOver: (self: Target, threadId: number) -> boolean,
	--- Steps out of the current function call on `threadId`.
	--- Returns `true` if target was paused before stepping and `false` otherwise.
	stepOut: (self: Target, threadId: number) -> boolean,
}

--- Create a new Target handle on the current VM.
function debugger.newTarget(): Target
	error("not implemented")
end

return debugger
