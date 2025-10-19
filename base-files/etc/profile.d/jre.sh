if [ -d "/usr/lib/jvm/java-17-openjdk" ]; then
    if [ ! -L "/usr/lib/jvm/default" ]; then
        ln -s /usr/lib/jvm/java-17-openjdk /usr/lib/jvm/default
    fi
    append_path '/usr/lib/jvm/default/bin'
    export PATH
fi
