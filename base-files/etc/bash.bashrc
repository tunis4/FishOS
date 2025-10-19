#
# /etc/bash.bashrc
#

# If not running interactively, don't do anything
[[ $- != *i* ]] && return

[[ $DISPLAY ]] && shopt -s checkwinsize

HISTCONTROL=ignoredups
HISTSIZE=-1
HISTFILESIZE=-1

PS1='\033[01;32mroot@\h\033[00m:\033[01;36m\w\033[00m\$ '

alias ls='ls --color=auto'
alias grep='grep --color=auto'
alias diff='diff --color=auto'

if [[ -r /usr/share/bash-completion/bash_completion ]] && ! shopt -oq posix; then
  . /usr/share/bash-completion/bash_completion
fi
