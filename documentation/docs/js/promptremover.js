document.addEventListener("DOMContentLoaded", function(){
    // get collection of code blocks:
    const collection = document.getElementsByClassName("highlight");
    for (let i = 0; i < collection.length; i++) {
        const commandElement=collection.item(i);
        let commandButtonElement = commandElement.getElementsByTagName("button");
        // read the prompt string from an attribute of the code block:
        let promptString = commandElement.getAttribute("data-prompt");
        if (!promptString) continue;
        let commandCodeElement = commandElement.getElementsByTagName("code");
        let commandCodeElementString = commandCodeElement.item(0).textContent;
        let trueCommand = commandCodeElementString;
        if (commandCodeElementString.startsWith(promptString)) {
        // remove the first occurrence of the prompt:
            trueCommand = commandCodeElementString.substring(promptString.length, commandCodeElementString.length).trim();
        }
        // remove other occurrencies in case of a multi-line string:
        trueCommand = trueCommand.replaceAll("\n"+promptString, "\n").replace(/^[^\S\r\n]+/gm, "");

        // CHECK IF THERE IS A SECOND PROMPT:
        promptString = commandElement.getAttribute("data-prompt-second");
        if (promptString) {
            if (trueCommand.startsWith(promptString)) {
                trueCommand = trueCommand.substring(promptString.length, trueCommand.length).trim();
            }
            trueCommand = trueCommand.replaceAll("\n"+promptString, "\n").replace(/^[^\S\r\n]+/gm, "");
        }

        // CHECK IF THERE IS A THIRD PROMPT:
        promptString = commandElement.getAttribute("data-prompt-third");
        if (promptString) {
            if (trueCommand.startsWith(promptString)) {
                trueCommand = trueCommand.substring(promptString.length, trueCommand.length).trim();
            }
            trueCommand = trueCommand.replaceAll("\n"+promptString, "\n").replace(/^[^\S\r\n]+/gm, "");
        }
        // attach the updated command as an attribute to the button where clipboard.js will find it:
        commandButtonElement.item(0).setAttribute("data-clipboard-text", trueCommand);
    }
});




