-- markpreview.lua - open the current file in smd as a live preview
local M = {}

M.opts = {
	cmd = "smd",
	filetypes = { "markdown", "html" },
}

local augroup = vim.api.nvim_create_augroup("MarkPreview", { clear = true })

local jobs = {}

vim.api.nvim_create_autocmd("VimLeavePre", {
	group = augroup,
	callback = function()
		for path in pairs(jobs) do
			M.close(path)
		end
	end,
})

local function running(path)
	local job = jobs[path]
	if not job then
		return false
	end
	return vim.fn.jobwait({ job }, 0)[1] == -1
end

function M.open(path)
	path = path or vim.api.nvim_buf_get_name(0)
	if path == "" then
		vim.notify("markpreview: buffer isn't a file yet", vim.log.levels.WARN)
		return
	end

	local ft = vim.bo.filetype
	if not vim.tbl_contains(M.opts.filetypes, ft) then
		vim.notify("markpreview: not a markdown/html buffer (filetype=" .. ft .. ")", vim.log.levels.WARN)
		return
	end

	if running(path) then
		return
	end

	local job = vim.fn.jobstart(
		{ "sh", "-c", string.format("exec %s %s </dev/null >/dev/null 2>&1", M.opts.cmd, vim.fn.shellescape(path)) },
		{
			detach = true,
			on_exit = function()
				jobs[path] = nil
			end,
		}
	)

	if job <= 0 then
		vim.notify("markpreview: couldn't start '" .. M.opts.cmd .. "'", vim.log.levels.ERROR)
		return
	end
	jobs[path] = job

	local winid = vim.api.nvim_get_current_win()
	vim.api.nvim_create_autocmd({ "BufDelete", "BufWipeout" }, {
		group = augroup,
		buffer = vim.api.nvim_get_current_buf(),
		once = true,
		callback = function()
			M.close(path)
		end,
	})
	vim.api.nvim_create_autocmd("WinClosed", {
		group = augroup,
		pattern = tostring(winid),
		once = true,
		callback = function()
			M.close(path)
		end,
	})
end

function M.close(path)
	path = path or vim.api.nvim_buf_get_name(0)
	local job = jobs[path]
	if job then
		vim.fn.jobstop(job)
		jobs[path] = nil
	end
end

function M.setup(opts)
	M.opts = vim.tbl_extend("force", M.opts, opts or {})
end

vim.api.nvim_create_user_command("MarkPreview", function()
	M.open()
end, {})

vim.api.nvim_create_user_command("MarkPreviewClose", function()
	M.close()
end, {})

-- vim.keymap.set("n", "<leader>mp", "<cmd>MarkPreview<cr>")

return M
