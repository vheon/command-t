local if_nil = function(x, y)
  if x == nil then return y end
  return x
end

local native_lua = require'commandt'
local sorters = require('telescope.sorters')

local function get_commandt_sorter()
  return sorters.Sorter:new {
    discard = true,
    scoring_function = function(_, prompt, line)
      local score = native_lua.score(line, prompt, false)
      if score == 0 then
        return -1
      end

      return 1 - score
    end
  }
end

return require('telescope').register_extension {
  setup = function(ext_config, config)
    local override_file = if_nil(ext_config.override_file_sorter, true)
    if override_file then
      config.file_sorter = function()
        return get_commandt_sorter()
      end
    end

    local override_generic = if_nil(ext_config.override_generic_sorter, true)
    if override_generic then
      config.generic_sorter = function()
        return get_commandt_sorter()
      end
    end
  end,

  -- requires = ...
  -- mappings = ...
  -- actions = ...
  -- commands = ...

  exports = {
    native_commandt_sorter = function()
      return get_commandt_sorter()
    end
  }
}
