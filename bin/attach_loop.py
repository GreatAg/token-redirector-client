import argparse
import subprocess
import random
import time

usbip_path = "C:\\Program Files\\APK Token Redirector Client\\usbip.exe"
random.seed()

def run(args):
        subprocess.run(args, stderr=subprocess.STDOUT, text=True)

def loop(usbip, remote, busid, max_delay, count):
        stop = max_delay + 1
        for i in range(count):
                print("#{}".format(i + 1))
                run([usbip, "attach", "--remote", remote, "--bus-id", busid])

                if stop > 1:
                        sec = random.randrange(stop)
                        if sec:
                                time.sleep(sec)

                run([usbip, "detach", "--all"])

def parse_args():
        p = argparse.ArgumentParser(description='usbip attach/detach loop',
                                    formatter_class=argparse.ArgumentDefaultsHelpFormatter)

        p.add_argument('-r', '--remote', type=str, dest='remote', metavar='HOST', required=True,
                        help='usbip server address')

        p.add_argument('-b', '--bus-id', type=str, dest='busid', metavar='ID', required=True,
                        help='bus-id of USB device')

        p.add_argument('-d', '--max-delay', type=int, default=3, dest='max_delay', metavar='SEC',
                        help='max delay before detach, seconds')

        p.add_argument('-p', '--program', type=str, default=usbip_path, dest='usbip', metavar='PATH',
                        help='path to usbip.exe')

        p.add_argument('count', type=int, default=0xFFFF, nargs='?', metavar='N', help='number of loops')

        return p.parse_args()

try:
        args = parse_args()
        loop(args.usbip, args.remote, args.busid, args.max_delay, args.count)
except KeyboardInterrupt:
        pass
except Exception as e:
        print(e)
