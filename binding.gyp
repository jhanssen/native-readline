{
  "targets": [
   {
      "include_dirs": [
        "/usr/local/Cellar/readline/7.0/include/",
        "<!(node -e \"require('nan')\")"
      ],
      "libraries": [
        "-L/usr/local/Cellar/readline/7.0/lib", "-lreadline"
      ],
      "target_name": "native-readline",
      "sources": [ "readline.cpp", "Redirector.cpp", "utils.cpp" ]
    }
  ]
}
