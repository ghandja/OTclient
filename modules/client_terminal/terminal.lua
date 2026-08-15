-- chunkname: @/modules/client_terminal/terminal.lua

local LogColors = {
	[LogDebug] = "pink",
	[LogInfo] = "white",
	[LogWarning] = "yellow",
	[LogError] = "red"
}
local MaxLogLines = 128
local terminalWindow, terminalButton, terminalBuffer, terminalSelectText
local logLocked = false
local poped = false
local oldPos, oldSize
local firstShown = false
local flushEvent
local cachedLines = {}
local disabled = false
local allLines = {}

local function onLog(level, message, time)
	if disabled or logLocked then
		return
	end

	logLocked = true

	addLine(message, LogColors[level])

	logLocked = false
end

function init()
	terminalWindow = g_ui.displayUI("terminal")

	terminalWindow:setVisible(false)

	terminalWindow.onDoubleClick = popWindow
	terminalButton = modules.client_topmenu.addTopRightToggleButton("terminalButton", tr("Terminal") .. " (Ctrl + T)", "/images/topbuttons/terminal", toggle)

	Keybind.new("Misc.", "Toggle Terminal", "Ctrl+T", "")
	Keybind.bind("Misc.", "Toggle Terminal", {
		{
			type = KEY_DOWN,
			callback = toggle
		}
	})

	terminalBuffer = terminalWindow:getChildById("terminalBuffer")
	terminalSelectText = terminalWindow:getChildById("terminalSelectText")
	terminalSelectText.onDoubleClick = popWindow

	function terminalSelectText.onMouseWheel(a, b, c)
		terminalBuffer:onMouseWheel(b, c)
	end

	function terminalBuffer:onScrollChange(value)
		terminalSelectText:setTextVirtualOffset(value)
	end

	g_keyboard.bindKeyDown("Escape", hide, terminalWindow)
	g_logger.setOnLog(onLog)

	if not g_app.isRunning() then
		g_logger.fireOldMessages()
	elseif _G.terminalLines then
		for _, line in pairs(_G.terminalLines) do
			addLine(line.text, line.color)
		end
	end
end

function terminate()
	removeEvent(flushEvent)

	if poped then
		oldPos = terminalWindow:getPosition()
		oldSize = terminalWindow:getSize()
	end

	local settings = {
		size = oldSize,
		pos = oldPos,
		poped = poped
	}

	g_settings.setNode("terminal-window", settings)
	Keybind.delete("Misc.", "Toggle Terminal")
	g_logger.setOnLog(nil)
	terminalWindow:destroy()
	terminalButton:destroy()

	terminalWindow = nil
	terminalButton = nil
	_G.terminalLines = allLines
end

function hideButton()
	terminalButton:hide()
end

function popWindow()
	if poped then
		oldPos = terminalWindow:getPosition()
		oldSize = terminalWindow:getSize()

		terminalWindow:fill("parent")
		terminalWindow:setOn(false)
		terminalWindow:getChildById("bottomResizeBorder"):disable()
		terminalWindow:getChildById("rightResizeBorder"):disable()
		terminalWindow:getChildById("titleBar"):hide()
		terminalWindow:getChildById("terminalScroll"):setMarginTop(0)
		terminalWindow:getChildById("terminalScroll"):setMarginBottom(0)
		terminalWindow:getChildById("terminalScroll"):setMarginRight(0)

		poped = false
	else
		terminalWindow:breakAnchors()
		terminalWindow:setOn(true)

		local size = oldSize or {
			width = g_window.getWidth() / 2.5,
			height = g_window.getHeight() / 4
		}

		terminalWindow:setSize(size)

		local pos = oldPos or {
			x = 0,
			y = g_window.getHeight()
		}

		terminalWindow:setPosition(pos)
		terminalWindow:getChildById("bottomResizeBorder"):enable()
		terminalWindow:getChildById("rightResizeBorder"):enable()
		terminalWindow:getChildById("titleBar"):show()
		terminalWindow:getChildById("terminalScroll"):setMarginTop(18)
		terminalWindow:getChildById("terminalScroll"):setMarginBottom(1)
		terminalWindow:getChildById("terminalScroll"):setMarginRight(1)
		terminalWindow:bindRectToParent()

		poped = true
	end
end

function toggle()
	if terminalWindow:isVisible() then
		hide()
	else
		if not firstShown then
			local settings = g_settings.getNode("terminal-window")

			if settings then
				if settings.size then
					oldSize = settings.size
				end

				if settings.pos then
					oldPos = settings.pos
				end

				if settings.poped then
					popWindow()
				end
			end

			firstShown = true
		end

		show()
	end
end

function show()
	terminalWindow:show()
	terminalWindow:raise()
	terminalWindow:focus()
	flushLines()
end

function hide()
	terminalWindow:hide()
end

function disable()
	terminalButton:hide()

	disabled = true
end

function flushLines()
	local fulltext = terminalSelectText:getText()
	local start = #cachedLines + 1 - MaxLogLines

	if start < 0 then
		start = 1
	end

	for i = start, #cachedLines do
		local line = cachedLines[i]

		if terminalBuffer:getChildCount() >= MaxLogLines then
			local firstChild = terminalBuffer:getChildByIndex(1)

			if firstChild then
				local len = #firstChild:getText()

				firstChild:destroy()
				table.remove(allLines, 1)

				fulltext = string.sub(fulltext, len)
			end
		end

		local label = g_ui.createWidget("TerminalLabel", terminalBuffer)

		label:setId("terminalLabel" .. i)
		label:setText(line.text)
		label:setColor(line.color)
		table.insert(allLines, {
			text = line.text,
			color = line.color
		})

		fulltext = fulltext .. "\n" .. line.text
	end

	terminalSelectText:setText(fulltext)

	cachedLines = {}

	removeEvent(flushEvent)

	flushEvent = nil
end

function addLine(text, color)
	text = string.gsub(text, "\t", "    ")

	table.insert(cachedLines, {
		text = text,
		color = color
	})

	if terminalWindow:isVisible() and not flushEvent then
		flushEvent = scheduleEvent(flushLines, 10)
	end
end

function clear()
	terminalBuffer:destroyChildren()
	terminalSelectText:setText("")

	cachedLines = {}
	allLines = {}
end
