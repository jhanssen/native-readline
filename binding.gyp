{
  "variables": {
    "conditions": [
      # Define variables that points at OS-specific paths.
      ["OS=='mac'", {
        "os_include": "/usr/local/opt/readline/include",
        "os_libs": "-L/usr/local/opt/readline/lib"
      }, {
        "os_include": "",
        "os_libs": ""
      }],
    ],
  },
  "targets": [
   {
      "include_dirs": [
        "<(os_include)",
        "<!(node -e \"require('nan')\")"
      ],
      "libraries": [
        "<(os_libs)",
        "-lreadline"
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
