<<<<<<< HEAD
-- while these types may have additional fields, we only record
-- the ones the DAP server currently uses.

export type Capabilitiy = {
	supportConfigurationDoneRequest: boolean?,
	supportTerminateRequest: boolean?,
}

export type Source = {
	path: string?,
=======
-- these types reflect our requirements for our implementation of DAP, not
-- necessarily the actual DAP specification. For example, sources in DAP could have
-- path be optional, but we enforce that it is required.

export type Capability = {
	supportConfigurationDoneRequest: boolean,
	supportTerminateRequest: boolean,
}

export type Source = {
	path: string,
>>>>>>> 1219ec54 (`lute/debug`: add prototype DAP adapter for breakpoints (#1251))
	name: string?,
}

export type SourceBreakpoint = {
	line: number,
}

export type SetBreakpointsArgs = {
	source: Source,
<<<<<<< HEAD
	breakpoints: { SourceBreakpoint }?,
}

export type Breakpoint = {
	id: number?,
	verified: boolean,
	source: Source?,
	reason: ("pending" | "failed")?,
	line: number?,
=======
	breakpoints: { SourceBreakpoint },
}

export type Breakpoint = {
	id: number,
	verified: boolean,
	source: Source,
	reason: ("pending" | "failed")?,
	line: number,
>>>>>>> 1219ec54 (`lute/debug`: add prototype DAP adapter for breakpoints (#1251))
}

export type LaunchArgs = {
	program: string,
	args: { string }?,
}

return {}
