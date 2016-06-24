FROM ubuntu:latest

ENV DEBIAN_FRONTEND noninteractive

RUN apt-get update && apt-get install -y mc wget build-essential libreadline6-dev zlib1g-dev liblzo2-dev mt-st mtx postfix libacl1-dev libssl-dev postgresql-server-dev-9.5 postgresql-client-9.5 && \
    wget -qO- http://bacula.us/741 | tar -xzvf - -C /usr/src && \
    cd /usr/src/bacula* && \
    ./configure --with-readline=/usr/include/readline --disable-conio --bindir=/usr/bin --sbindir=/usr/sbin \
    --with-scriptdir=/etc/bacula/scripts --with-working-dir=/var/lib/bacula --with-logdir=/var/log \
    --enable-smartalloc --with-postgresql --with-archivedir=/mnt/backup && \
    make -j8 && make install

RUN apt-get install sudo

ADD create_postgresql_database /etc/bacula/scripts

