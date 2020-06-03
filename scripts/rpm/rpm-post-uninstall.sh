# rpm-post-uninstall.sh
# rpm will execute the contents of this file with /bin/sh
# https://fedoraproject.org/wiki/Packaging:Scriptlets

if [[ $1 -eq 0 ]]; then
    # this is a real uninstall, NOT an upgrade

    rm -fr /var/log/hse
    rm -fr /etc/logrotate.d/hse
fi

exit 0
