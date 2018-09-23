# Snowball tokenizer for SQLite FTS5

This is a simple extension for use with FTS5 within SQLite. It allows FTS5 to use Martin Porter's Snowball stemmers (libstemmer), which are available in several languages. Check http://snowballstem.org/ for more information about them.

# Usage

This is an [SQLite](http://sqlite.org/) plugin. It's meant to be used with the [FTS5 extension](https://www.sqlite.org/fts5.html).

The usage is very similar to the stock Porter tokenizer:

```
CREATE VIRTUAL TABLE mail USING fts5(sender, title, body, tokenize = 'snowball');
```

The main difference is that you can pass the language as the second parameter:
```
CREATE VIRTUAL TABLE books USING fts5(author, title, description, tokenize = 'snowball spanish');
```

You can even pass several languages, and they will be tried in order until one of them works:
```
CREATE VIRTUAL TABLE books USING fts5(author, title, description, tokenize = 'snowball spanish english');
```

After the language(s), you can pass the other parameters you would pass to the stock porter tokenizer:
```
CREATE VIRTUAL TABLE mail USING fts5(sender, title, body, tokenize = "snowball english unicode61 tokenchars '-_'");
```

Those are the main differences. The data can be queried in the same fashion as any other FTS5 table.

# Building

This needs GNU make in order to build. If you're using BSD or similar, you can always get gmake.

Current version requires SQLite 3.20.0 or newer.

Before building, the Snowball code needs to be put inside a directory called _snowball_ in the base directory. The easiest way to get it is by doing:

```
git clone https://github.com/snowballstem/snowball.git
```

After that, just run make (or gmake), and you should obtain the final file called _fts5stemmer.so_

# Loading

To use this plugin from within the sqlite3 command line tool, you must run:

```.load path-to-plugin/fts5stemmer``` (for security reasons, SQLite needs you to provide the library path if the plugin is not located inside ```LD_LIBRARY_PATH```. Current directory '.' is a valid path, e.g., ```.load ./fts5stemmer```).

You could also use this from within almost any code that uses sqlite3 as a library (untested). Refer to SQLite website to get more information about ```sqlite3_load_extension``` and ```sqlite3_enable_load_extension``` or look for their equivalents in your language bindings' documentation.

# License

The work here contained is published under the BSD 3-Clause License. Read LICENSE for more information.
