{
  "variables": {
    "os_include": "3rdparty/readline-install/include",
    "os_libs": "../3rdparty/readline-install/lib/libreadline.a ../3rdparty/readline-install/lib/libhistory.a"
  },
  "targets": [
   {
      "include_dirs": [
        "<(os_include)",
        "<!(node -e \"require('nan')\")"
      ],
      "libraries": [
        "<(os_libs)",
        "-ltinfo"
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
