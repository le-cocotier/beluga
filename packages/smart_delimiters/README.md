# Smart delimiters

Smart delimiters is a package for beluga editor. The aim of it is to add closure delimiter when opening it.

## Install
Clone this repo in the packages folder of beluga (`~/.beluga/packages`).
``` bash
git clone https://homelinuxserver.ddns.net/git/arthur/smart_delimiters.git ~/.beluga/packages/smart_delimiters
```
Then add the following line to your init file.

``` lisp
(add-package "smart_delimiters")
```

## Functions

| Function | Description |
|:---------|:------------|
| `open-parenthesis` | insert open and close parenthensis then put the cursor in between |
| `open-brackets` | insert open and close brackets then put the cursor in between |
| `open-curly-brackets` | insert open and close curly brackets then put the cursor in between |
| `open-single-quotes` | insert two single quotes then put the cursor in between |
| `open-double-quotes` | insert two double quotes then put the cursor in between |

## Binding

All functions are binded to the corresponding key.

