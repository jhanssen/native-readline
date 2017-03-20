{
  "variables": {
    "rl_include": "3rdparty/readline-install/include",
    "rl_libs": "../3rdparty/readline-install/lib/libreadline.a ../3rdparty/readline-install/lib/libhistory.a",
    "conditions": [
      # Define variables that points at OS-specific paths.
      ["OS=='mac'", {
        "os_libs": ""
      }, {
        "os_libs": "-ltinfo"
      }],
    ]
  },
  "targets": [
   {
      "include_dirs": [
        "<(rl_include)",
        "<!(node -e \"require('nan')\")"
      ],
      "libraries": [
        "<(rl_libs)",
        "<(os_libs)"
      ],
      "target_name": "native-readline",
      "sources": [ "readline.cpp", "Redirector.cpp", "utils.cpp" ],
      "cflags_cc": [ "-std=c++14" ],
      "xcode_settings": {
	"OTHER_CPLUSPLUSFLAGS": [
	  "-std=c++14"
	]
      }
    }
  ]
}
