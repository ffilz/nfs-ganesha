#!/usr/bin/env python

import re, sys
import pyparsing as pp
import logging, pprint

LBRACE = pp.Literal("{").suppress()
RBRACE = pp.Literal("}").suppress()
SEMICOLON = pp.Literal(";").suppress()
EQUAL = pp.Literal("=").suppress()
KEY = pp.Word(pp.alphas, pp.alphanums+"_")
VALUE = pp.Word(pp.printables, excludeChars=';')
BLOCKNAME = pp.Word(pp.alphas, pp.alphanums+"_")
KEYPAIR = KEY + EQUAL + VALUE + SEMICOLON

# block is recursively defined. It starts with name, left brace, then a
# list of "key value" pairs followed by a list of blocks, and finally
# ends with a right brace. We assume that all "key value" pairs ,if any,
# precede any sub-blocks. Ganesha daemon itself allows "key value" pairs
# after sub-blocks or in between sub-blocks, but we don't allow for
# simplification.
#
# We contruct a 3 element list in python for every block! The first
# element is the name of block itself, the second element is the list of
# "key value" pairs, and the last element is the list of sub-block. The
# first element must be the name of the block and the remaining two
# elements could be empty lists! This recursive list is usually named as
# r3.

# block definition for pyparsing.
ppblock = pp.Forward()
KEYPAIR_GROUP = pp.Group(pp.ZeroOrMore(pp.Group(KEYPAIR)))
SUBS_GROUP = pp.Group(pp.ZeroOrMore(pp.Group(ppblock)))
ppblock << BLOCKNAME + LBRACE + KEYPAIR_GROUP + SUBS_GROUP + RBRACE

# Given a block as recursive 3 element list, and the indentation level,
# produce a corresponding text that could be written to the config file!
def r3_to_text(r3, level):
    logging.debug("%s", pprint.pformat(r3))
    if not r3:
        return ""
    name, keypairs, subs = r3[0], r3[1], r3[2]
    indent = level * "\t"
    s = indent + name + " {\n"
    for keypair in keypairs:
        key, value = keypair[0], keypair[1]
        s += indent + "\t" + "%s = %s;\n" % (key, value)
    for sub in subs:
        s += r3_to_text(sub, level+1)
    s += indent + "}\n"
    return s

# @todo: this needs more validation specific to ganesha config blocks
def blocknames_valid(blocknames):
    if not blocknames:
        return False

    while blocknames:
        if not blockname_valid(blocknames):
            return False
        blocknames = next_subnames(blocknames)
        # @todo make sure that this subblock is valid for ganesha config

    return True

def blockname_valid(blocknames):
    if not blocknames:
        return False

    # export and client blocks require a key and a value to identify them
    if blocknames[0].lower() == "export" and len(blocknames) < 3:
        return False
    if blocknames[0].lower() == "client" and len(blocknames) < 3:
        return False

    return True

def next_subnames(blocknames):
    assert blocknames
    if blocknames[0].lower() == "export" or blocknames[0].lower() == "client":
        # export and client blocks require a key and a value to identify them
        return blocknames[3:]
    else:
        return blocknames[1:]

def block_match(blocknames, name, pairs):
    logging.debug("names:%s, name:%s, pairs:%s",
            pprint.pformat(blocknames), name, pprint.pformat(pairs))
    if blocknames[0].lower() == "export" or blocknames[0].lower() == "client":
        if blocknames[0].lower() != name.lower():
            return False

        key = blocknames[1]
        value = blocknames[2]

        if blocknames[0].lower() == "export":
            valid_keys = ["export_id", "pseudo"]
        else:
            valid_keys = ["clients"]

        if key.lower() not in valid_keys:
            return False

        for pair in pairs:
            if pair[0].lower() == key.lower() and pair[1] == value:
                return True
        return False

    return blocknames[0].lower() == name.lower() # neither export nor client block

def make_r3(blocknames):
    if blocknames[0].lower() == "export" or blocknames[0].lower() == "client":
        pairs = [[blocknames[1], blocknames[2]]]
    else:
        pairs = []

    return [blocknames[0], pairs, []]

class BLOCK(object):
    def __init__(self, blocknames):
        if not blocknames_valid(blocknames):
            sys.exit("not a valid block")
        self.blocknames = blocknames

    def set_keys(self, s, opairs):
        match = ppblock.parseWithTabs().scanString(s)
        block_found = False
        for ppr, start, end in match:
            if block_match(self.blocknames, ppr[0], ppr[1]):
                block_found = True
                break;

        if block_found:
            begin_part = s[:start]
            end_part = s[end:]
            r3 = ppr.asList()
            logging.debug("%s", pprint.pformat(r3))
            self.set_process(r3, self.blocknames, opairs)
            text = r3_to_text(r3, 0)
            logging.debug("%s", pprint.pformat(text))
            assert text[-1] == "\n"
            if end_part[0] == "\n":
                text = text[:-1] # remove the last new line
        else:
            begin_part = s
            end_part = ""
            r3 = make_r3(self.blocknames)
            self.set_process(r3, self.blocknames, opairs)
            text = r3_to_text(r3, 0)

        return begin_part + text + end_part

    def del_keys(self, s, okeys):
        if len(okeys) > 1:
            sys.exit("more than one key is unsupported yet")

        match = ppblock.parseWithTabs().scanString(s)
        block_found = False
        for ppr, start, end in match:
            if block_match(self.blocknames, ppr[0], ppr[1]):
                block_found = True
                break;

        if block_found:
            begin_part = s[:start]
            end_part = s[end:]
            r3 = ppr.asList()
            logging.debug("%s", pprint.pformat(r3))
            self.del_process(r3, self.blocknames, okeys)
            text = r3_to_text(r3, 0)
            logging.debug("%s", pprint.pformat(text))

            # if we remove this entire block, remove the last new line
            # character associated with this block.
            #
            # @todo: should we remove other white space also?
            if end_part[0] == "\n":
                end_part = end_part[1:]
        else:
            logging.debug("block not found")
            sys.exit("block not found")

        return begin_part + text + end_part

    def set_process(self, r3, blocknames, opairs):
        logging.debug("names: %s, r3: %s", pprint.pformat(blocknames),
                      pprint.pformat(r3))
        name, pairs, subs = r3[0], r3[1], r3[2]
        assert block_match(blocknames, name, pairs)

        # If last block, add given key value opairs
        subnames = next_subnames(blocknames)
        if not subnames:
            for key, value in opairs:
                key_found = False
                for idx, pair in enumerate(pairs):
                    if key.lower() == pair[0].lower():
                        key_found = True
                        pairs[idx] = [key, value]
                if not key_found:
                    pairs.append([key, value])
            return

        block_found = False
        for sub in subs:
            name2, pairs2, subs2 = sub[0], sub[1], sub[2]
            if block_match(subnames, name2, pairs2):
                block_found = True
                break;

        if block_found:
            self.set_process(sub, subnames, opairs)
        else:
            new_r3 = make_r3(subnames)
            subs.append(new_r3)
            self.set_process(new_r3, subnames, opairs)

    def del_process(self, r3, blocknames, okeys):
        logging.debug("names: %s, r3: %s", pprint.pformat(blocknames),
                      pprint.pformat(r3))
        name, pairs, subs = r3[0], r3[1], r3[2]

        assert block_match(blocknames, name, pairs)

        # If last block, delete given okeys
        subnames = next_subnames(blocknames)
        if not subnames:
            for key in okeys:
                key_found = False
                for pair in pairs[:]:
                    if key.lower() == pair[0].lower():
                        key_found = True
                        pairs.remove(pair)
                if not key_found: # @todo: exception to report
                    sys.exit("key to delete is not found")

            # export and client blocks can't exist without some
            # key pairs identifying them. So remove the whole
            # block. @todo: shall we do this for regular blocks
            # also?
            if not pairs and (blocknames[0].lower() == "export" or
                              blocknames[0].lower() == "client"):
                r3[:] = []
            if not okeys: # remove the whole block
                r3[:] = []

            return

        block_found = False
        for sub in subs:
            name, keypairs, subs2 = sub[0], sub[1], sub[2]
            if block_match(subnames, name, keypairs):
                block_found = True
                break

        if block_found:
            self.del_process(sub, subnames, okeys)
        else:
            logging.debug("block not found")
            sys.exit("block not found")
