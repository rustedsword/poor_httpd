fs = import('fs')
python = import('python').find_installation()

foreach line : fs.read('test_httpd.py').split('\n')
  if line.startswith('def test_')
    name = line.substring(9).split('(')[0]
    test(name, python, args: [files('test_httpd.py'), poor_httpd, name], timeout: 120)
  endif
endforeach
