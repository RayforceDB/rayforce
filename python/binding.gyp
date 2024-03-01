{
  "targets": [
    {
      "target_name": "rayforce",
      "sources": [ "rayforce_wrap.cxx" ],
      "include_dirs": [ "../core/" ],
      "libraries": [ "-L../../", "-lrayforce" ],
      "conditions": [
        ['OS=="linux"', {
            "cflags": [ "-std=c++11" ],
            "cflags_cc": [ "-std=c++11" ]
        }]],
    }
  ],
  "make_global_settings": [
    ["CC", "/usr/bin/g++"],
    ["CXX", "/usr/bin/g++"],
    ["LINK", "/usr/bin/g++"]
  ]

}
