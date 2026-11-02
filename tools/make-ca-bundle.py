# make-ca-bundle.py <out.h>: the system's Mozilla roots as a C string for http_ps3.c
import sys

pem = open("/etc/ssl/certs/ca-certificates.crt").read()
with open(sys.argv[1], "w") as f:
    f.write("// made by CI from /etc/ssl/certs/ca-certificates.crt\nstatic const char ca_bundle_pem[] =\n")
    for line in pem.splitlines():
        f.write('"%s\\n"\n' % line.replace('"', ''))
    f.write(";\n")
print(pem.count("BEGIN CERTIFICATE"), "root certificates")
