//
// Copyright (c) 2012 The ANGLE Project Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//

#ifndef COMPILER_PREPROCESSOR_MACROEXPANDER_H_
#define COMPILER_PREPROCESSOR_MACROEXPANDER_H_

#include <cassert>
#include <memory>
#include <vector>

#include "Lexer.h"
#include "Macro.h"
#include "pp_utils.h"

namespace pp
{

class Diagnostics;
struct SourceLocation;

class MacroExpander : public Lexer
{
  public:
    // allowedDepth: how many more argument pre-expansions may nest below this one (see collectMacroArgs).
    MacroExpander(Lexer *lexer, MacroSet *macroSet, Diagnostics *diagnostics,
                  int allowedDepth = kMaxNestedArgExpansion, size_t *sharedBudget = nullptr);
    static const int kMaxNestedArgExpansion = 256;
    // Jihad: the tokens all expansions of one shader may produce, shared with the expanders nested in it (a small
    // shader of nested D(x) x x calls produced tens of millions: Codex review 2026-11-19)
    static const size_t kMaxExpandedTokens = 100000;
    ~MacroExpander() override;

    void lex(Token *token) override;

  private:
    PP_DISALLOW_COPY_AND_ASSIGN(MacroExpander);

    void getToken(Token *token);
    void ungetToken(const Token &token);
    bool isNextTokenLeftParen();

    bool pushMacro(const Macro &macro, const Token &identifier);
    void popMacro();

    bool expandMacro(const Macro &macro,
                     const Token &identifier,
                     std::vector<Token> *replacements);

    typedef std::vector<Token> MacroArg;
    bool collectMacroArgs(const Macro &macro,
                          const Token &identifier,
                          std::vector<MacroArg> *args,
                          SourceLocation *closingParenthesisLocation);
    bool spend(size_t n);
    bool replaceMacroParams(const Macro &macro,
                            const std::vector<MacroArg> &args,
                            std::vector<Token> *replacements);

    struct MacroContext
    {
        const Macro *macro;
        std::size_t index;
        std::vector<Token> replacements;

        MacroContext()
            : macro(0),
              index(0)
        {
        }
        bool empty() const
        {
            return index == replacements.size();
        }
        const Token &get()
        {
            return replacements[index++];
        }
        void unget()
        {
            assert(index > 0);
            --index;
        }
    };

    Lexer *mLexer;
    MacroSet *mMacroSet;
    Diagnostics *mDiagnostics;
    int mAllowedDepth;
    size_t mOwnBudget;
    size_t *mBudget;

    std::unique_ptr<Token> mReserveToken;
    std::vector<MacroContext *> mContextStack;
};

}  // namespace pp

#endif  // COMPILER_PREPROCESSOR_MACROEXPANDER_H_
