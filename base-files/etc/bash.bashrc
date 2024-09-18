# If not running interactively, don't do anything
[[ $- != *i* ]] && return

PS1='\033[01;32mroot@\h\033[00m:\033[01;36m\w\033[00m\$ '
alias ls="ls --color=auto"
