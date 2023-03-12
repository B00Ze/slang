//------------------------------------------------------------------------------
//! @file RegisterHasNoResetTest.h
//! @brief Test for the RegisterHasNoReset check
//
// SPDX-FileCopyrightText: Michael Popoloski
// SPDX-License-Identifier: MIT
//------------------------------------------------------------------------------
#include "Test.h"
#include "TidyFactory.h"

TEST_CASE("RegisterHasNoReset: Register not assigned on reset") {
    auto tree = SyntaxTree::fromText(R"(
module top;
    logic clk;
    logic rst_ni;
    logic a, b;

    always_ff @(posedge clk or negedge rst_ni) begin
        if (~rst_ni) begin
            a <= '0;
        end else begin
            a <= 1'b1;
            b <= 1'b1;
        end
    end
endmodule
)");

    Compilation compilation;
    compilation.addSyntaxTree(tree);
    compilation.getAllDiagnostics();
    auto& root = compilation.getRoot();

    Registry::initialize_default_check_config();
    auto visitor = Registry::create("RegisterHasNoReset");
    bool result = visitor->check(root);
    CHECK(result == false);
}

TEST_CASE("RegisterHasNoReset: Register always assigned") {
    auto tree = SyntaxTree::fromText(R"(
module top;
    logic clk;
    logic rst_ni;
    logic a;

    always_ff @(posedge clk or negedge rst_ni) begin
        if (~rst_ni) begin
            a <= '0;
        end else begin
            a <= 1'b1;
        end
    end
endmodule
)");

    Compilation compilation;
    compilation.addSyntaxTree(tree);
    compilation.getAllDiagnostics();
    auto& root = compilation.getRoot();

    Registry::initialize_default_check_config();
    auto visitor = Registry::create("RegisterHasNoReset");
    bool result = visitor->check(root);
    CHECK(result == true);
}

TEST_CASE("RegisterHasNoReset: Register always assigned outside if reset block") {
    auto tree = SyntaxTree::fromText(R"(
module top;
    logic clk;
    logic rst_ni;
    logic a, b;

    always_ff @(posedge clk or negedge rst_ni) begin
        if (~rst_ni) begin
            a <= '0;
        end else begin
            a <= 1'b1;
            b <= 1'b1;
        end
        b <= 1'b1;
    end
endmodule
)");

    Compilation compilation;
    compilation.addSyntaxTree(tree);
    compilation.getAllDiagnostics();
    auto& root = compilation.getRoot();

    Registry::initialize_default_check_config();
    auto visitor = Registry::create("RegisterHasNoReset");
    bool result = visitor->check(root);
    CHECK(result == true);
}

TEST_CASE("RegisterHasNoReset: Array always assigned") {
    auto tree = SyntaxTree::fromText(R"(
module top;
    logic clk;
    logic rst_ni;
    logic push, ptr;
    logic [2:0] data;
    logic [4:0][2:0] fifo;

    always_ff @(posedge clk or negedge rst_ni) begin
        if (~rst_ni) begin
            fifo <= '0;
        end
        else if (push) begin
            for (int i = 0; i <= 4; i++) begin
                if (ptr == i) fifo[i] <= data;
            end
        end
    end
endmodule
)");

    Compilation compilation;
    compilation.addSyntaxTree(tree);
    compilation.getAllDiagnostics();
    auto& root = compilation.getRoot();

    Registry::initialize_default_check_config();
    auto visitor = Registry::create("RegisterHasNoReset");
    bool result = visitor->check(root);
    CHECK(result == true);
}

TEST_CASE("RegisterHasNoReset: Array not assigned on reset") {
    auto tree = SyntaxTree::fromText(R"(
module top;
    logic clk;
    logic rst_ni;
    logic push, ptr;
    logic [2:0] data;
    logic [4:0][2:0] fifo;

    always_ff @(posedge clk or negedge rst_ni) begin
        if (~rst_ni) begin
        end
        else if (push) begin
            for (int i = 0; i <= 4; i++) begin
                fifo[i] <= data;
            end
        end
    end
endmodule
)");

    Compilation compilation;
    compilation.addSyntaxTree(tree);
    compilation.getAllDiagnostics();
    auto& root = compilation.getRoot();

    Registry::initialize_default_check_config();
    auto visitor = Registry::create("RegisterHasNoReset");
    bool result = visitor->check(root);
    CHECK(result == false);
}

TEST_CASE("RegisterHasNoReset: Struct always assigned") {
    auto tree = SyntaxTree::fromText(R"(
module top;
    logic clk;
    logic rst_ni;
    struct {
        logic a;
        logic b;
        logic c;
    } data;

    always_ff @(posedge clk or negedge rst_ni) begin
        if (~rst_ni) begin
            data.a <= 1'b0;
            data.b <= 1'b0;
            data.c <= 1'b0;
        end
        else begin
            data.a <= 1'b1;
        end
    end
endmodule
)");

    Compilation compilation;
    compilation.addSyntaxTree(tree);
    compilation.getAllDiagnostics();
    auto& root = compilation.getRoot();

    Registry::initialize_default_check_config();
    auto visitor = Registry::create("RegisterHasNoReset");
    bool result = visitor->check(root);
    CHECK(result == true);
}

TEST_CASE("RegisterHasNoReset: Struct not assigned on reset") {
    auto tree = SyntaxTree::fromText(R"(
module top;
    logic clk;
    logic rst_ni;
    struct {
        logic a;
        logic b;
        logic c;
    } data;

    always_ff @(posedge clk or negedge rst_ni) begin
        if (~rst_ni) begin
        end
        else begin
            data.a <= 1'b1;
        end
    end
endmodule
)");

    Compilation compilation;
    compilation.addSyntaxTree(tree);
    compilation.getAllDiagnostics();
    auto& root = compilation.getRoot();

    Registry::initialize_default_check_config();
    auto visitor = Registry::create("RegisterHasNoReset");
    bool result = visitor->check(root);
    CHECK(result == false);
}