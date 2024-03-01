{
  "targets": [
    {
      "target_name": "rayforce",
      "sources": [ "rayforce_wrap.c" ],
      "include_dirs": [ "../core/" ],
      "libraries": [ "-L../../", "-lrayforce" ],
      # "include_dirs": [ "<!(node -e \"require('node-addon-api').include\")" ],
      "cflags_cc": [ "-std=c++17" ],
      "cflags": [ "-std=c++17" ],
      "conditions": [
        ['OS=="mac"', {
          'xcode_settings': {
            'CLANG_CXX_LANGUAGE_STANDARD': 'c++17',
            'GCC_ENABLE_CPP_EXCEPTIONS': 'YES',
            'MACOSX_DEPLOYMENT_TARGET': '10.7',
          }
        }]
      ]
    }
  ]

}
