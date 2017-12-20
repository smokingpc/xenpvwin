#!python -u

import glob
import os
import shutil
import subprocess
import sys
import tarfile
import tempfile
import urllib.request
import zipfile

def shell(cmd):
    sys.stdout.flush()
    
    sub = subprocess.Popen(' '.join(cmd),
                           stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT)

    for line in sub.stdout:
        print(line.decode(sys.getdefaultencoding()).rstrip())

    sub.wait()

    return sub.returncode


def signtool(cert, files):
    path = [os.environ['KIT'], 'bin']
    if os.environ['PROCESSOR_ARCHITECTURE'] == 'x86':
        path.append('x86')
    else:
        path.append('x64')
    path.append('signtool.exe')

    cmd = ['"' + os.path.join(*path) + '"']
    cmd.append('sign')
    cmd.append('/v')
    cmd.append('/ac')
    cmd.append('"' + cert + '"')
    cmd.append('/a')
    cmd.append('/tr')
    cmd.append('http://timestamp.digicert.com')
    cmd.append('/td')
    cmd.append('sha256')
    cmd.append('/fd')
    cmd.append('sha256')
    cmd.append(' '.join(files))

    print("signing...")

    shell(cmd)


def report(count, block_size, total_size):
    current_size = count * block_size

    current_size >>= 10
    total_size >>= 10

    print("%uk/%uk" % (current_size, total_size), end='\r')


def fetch_cert():
    url = 'http://download.microsoft.com/download/2/4/E/24E730E6-C012-448F-92B6-78744D3B77E1/DigiCert%20High%20Assurance%20EV%20Root%20CA.zip'
    cert = 'DigiCert High Assurance EV Root CA.crt'

    print("fetching %s..." % cert)

    tmp = tempfile.NamedTemporaryFile(delete=False)
    tmp.close();

    urllib.request.urlretrieve(url, tmp.name)
    zip = zipfile.ZipFile(tmp.name)
    zip.extract(cert)
    zip.close()

    os.unlink(tmp.name);

    return cert


def fetch_unsigned_package(name, version):
    package = name + '.tar'
    url = 'https://xenbits.xen.org/pvdrivers/win/' + version + '/' + package

    print("fetching %s/%s..." % (version, package))

    urllib.request.urlretrieve(url, package, report)

    tmp = tempfile.TemporaryDirectory()

    unsigned = tarfile.open(package, 'r')
    unsigned.extractall(path=tmp.name)
    unsigned.close()

    os.unlink(package)

    return tmp


def create_signed_package(path, name, tmp):
    package = os.path.join(path, name + '.tar')

    print("creating %s..." % package)

    signed = tarfile.open(package, 'w')

    cwd = os.getcwd()
    os.chdir(tmp.name)

    signed.add(name, recursive=True)

    os.chdir(cwd)

    signed.close()


def sign_package(path, name, version):
    os.makedirs(path, exist_ok=True)

    tmp = fetch_unsigned_package(name, version)

    cwd = os.getcwd()
    os.chdir(tmp.name)

    files = glob.glob('**/*.cat', recursive=True)
    files += glob.glob('**/*.dll', recursive=True)
    files += glob.glob('**/*.exe', recursive=True)
    files += glob.glob('**/*.sys', recursive=True)

    cert = fetch_cert()
    signtool(cert, files)
    os.unlink(cert)

    os.chdir(cwd)

    create_signed_package(path, name, tmp)

    tmp.cleanup()

def main(argv):
    sign_package(argv[1], argv[2], argv[3])

if __name__ == '__main__':
    main(sys.argv)
